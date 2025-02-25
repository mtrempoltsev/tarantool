/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "replication.h"

#include <fiber.h> /* &cord->slabc */
#include <fiber_channel.h>
#include <scoped_guard.h>
#include <small/mempool.h>

#include "box.h"
#include "gc.h"
#include "error.h"
#include "relay.h"
#include "vclock.h" /* VCLOCK_MAX */
#include "sio.h"

uint32_t instance_id = REPLICA_ID_NIL;
struct tt_uuid INSTANCE_UUID;
struct tt_uuid REPLICASET_UUID;

double replication_timeout = 1.0; /* seconds */
double replication_connect_timeout = 30.0; /* seconds */
int replication_connect_quorum = REPLICATION_CONNECT_QUORUM_ALL;
double replication_sync_lag = 10.0; /* seconds */
double replication_sync_timeout = 300.0; /* seconds */
bool replication_skip_conflict = false;

struct replicaset replicaset;

static int
replica_compare_by_uuid(const struct replica *a, const struct replica *b)
{
	return tt_uuid_compare(&a->uuid, &b->uuid);
}

rb_gen(MAYBE_UNUSED static, replica_hash_, replica_hash_t,
       struct replica, in_hash, replica_compare_by_uuid);

#define replica_hash_foreach_safe(hash, item, next) \
	for (item = replica_hash_first(hash); \
	     item != NULL && ((next = replica_hash_next(hash, item)) || 1); \
	     item = next)

/**
 * Return the number of replicas that have to be synchronized
 * in order to form a quorum in the replica set.
 */
static inline int
replicaset_quorum(void)
{
	return MIN(replication_connect_quorum, replicaset.applier.total);
}

void
replication_init(void)
{
	memset(&replicaset, 0, sizeof(replicaset));
	replica_hash_new(&replicaset.hash);
	rlist_create(&replicaset.anon);
	vclock_create(&replicaset.vclock);
	fiber_cond_create(&replicaset.applier.cond);
	replicaset.replica_by_id = (struct replica **)calloc(VCLOCK_MAX, sizeof(struct replica *));
	latch_create(&replicaset.applier.order_latch);

	vclock_create(&replicaset.applier.vclock);
	vclock_copy(&replicaset.applier.vclock, &replicaset.vclock);
	rlist_create(&replicaset.applier.on_rollback);
	rlist_create(&replicaset.applier.on_commit);

	diag_create(&replicaset.applier.diag);
}

void
replication_free(void)
{
	/*
	 * Relay threads keep sending messages to tx via
	 * cbus upon shutdown, which could lead to segfaults.
	 * So cancel them.
	 */
	replicaset_foreach(replica)
		relay_cancel(replica->relay);

	diag_destroy(&replicaset.applier.diag);
	free(replicaset.replica_by_id);
}

int
replica_check_id(uint32_t replica_id)
{
	if (replica_id == REPLICA_ID_NIL) {
		diag_set(ClientError, ER_REPLICA_ID_IS_RESERVED,
			  (unsigned) replica_id);
		return -1;
	}
	if (replica_id >= VCLOCK_MAX) {
		diag_set(ClientError, ER_REPLICA_MAX,
			  (unsigned) replica_id);
		return -1;
	}
	/*
	 * It's okay to update the instance id while it is joining to
	 * a cluster as long as the id is set by the time bootstrap is
	 * complete, which is checked in box_cfg() anyway.
	 *
	 * For example, the replica could be deleted from the _cluster
	 * space on the master manually before rebootstrap, in which
	 * case it will replay this operation during the final join
	 * stage.
	 */
	if (!replicaset.is_joining && replica_id == instance_id) {
		diag_set(ClientError, ER_LOCAL_INSTANCE_ID_IS_READ_ONLY,
			  (unsigned) replica_id);
		return -1;
	}
	return 0;
}

/* Return true if replica doesn't have id, relay and applier */
static bool
replica_is_orphan(struct replica *replica)
{
	assert(replica->relay != NULL);
	return replica->id == REPLICA_ID_NIL && replica->applier == NULL &&
	       relay_get_state(replica->relay) != RELAY_FOLLOW;
}

static int
replica_on_applier_state_f(struct trigger *trigger, void *event);

static struct replica *
replica_new(void)
{
	struct replica *replica = (struct replica *)
			malloc(sizeof(struct replica));
	if (replica == NULL) {
		tnt_raise(OutOfMemory, sizeof(*replica), "malloc",
			  "struct replica");
	}
	replica->relay = relay_new(replica);
	if (replica->relay == NULL) {
		free(replica);
		diag_raise();
	}
	replica->id = 0;
	replica->uuid = uuid_nil;
	replica->applier = NULL;
	replica->gc = NULL;
	rlist_create(&replica->in_anon);
	trigger_create(&replica->on_applier_state,
		       replica_on_applier_state_f, NULL, NULL);
	replica->applier_sync_state = APPLIER_DISCONNECTED;
	latch_create(&replica->order_latch);
	return replica;
}

static void
replica_delete(struct replica *replica)
{
	assert(replica_is_orphan(replica));
	if (replica->relay != NULL)
		relay_delete(replica->relay);
	if (replica->gc != NULL)
		gc_consumer_unregister(replica->gc);
	TRASH(replica);
	free(replica);
}

struct replica *
replicaset_add(uint32_t replica_id, const struct tt_uuid *replica_uuid)
{
	assert(!tt_uuid_is_nil(replica_uuid));
	assert(replica_id != REPLICA_ID_NIL && replica_id < VCLOCK_MAX);

	assert(replica_by_uuid(replica_uuid) == NULL);
	struct replica *replica = replica_new();
	replica->uuid = *replica_uuid;
	replica_hash_insert(&replicaset.hash, replica);
	replica_set_id(replica, replica_id);
	return replica;
}

void
replica_set_id(struct replica *replica, uint32_t replica_id)
{
	assert(replica_id < VCLOCK_MAX);
	assert(replica->id == REPLICA_ID_NIL); /* replica id is read-only */
	replica->id = replica_id;

	if (tt_uuid_is_equal(&INSTANCE_UUID, &replica->uuid)) {
		/* Assign local replica id */
		assert(instance_id == REPLICA_ID_NIL);
		instance_id = replica_id;
	}
	replicaset.replica_by_id[replica_id] = replica;

	say_info("assigned id %d to replica %s",
		 replica->id, tt_uuid_str(&replica->uuid));
}

void
replica_clear_id(struct replica *replica)
{
	assert(replica->id != REPLICA_ID_NIL);
	/*
	 * Don't remove replicas from vclock here.
	 * The vclock_sum() must always grow, it is a core invariant of
	 * the recovery subsystem. Further attempts to register a replica
	 * with the removed replica_id will re-use LSN from the last value.
	 * Replicas with LSN == 0 also can't not be safely removed.
	 * Some records may arrive later on due to asynchronous nature of
	 * replication.
	 */
	replicaset.replica_by_id[replica->id] = NULL;
	if (replica->id == instance_id) {
		/* See replica_check_id(). */
		assert(replicaset.is_joining);
		instance_id = REPLICA_ID_NIL;
	}
	replica->id = REPLICA_ID_NIL;
	say_info("removed replica %s", tt_uuid_str(&replica->uuid));

	/*
	 * The replica will never resubscribe so we don't need to keep
	 * WALs for it anymore. Unregister it with the garbage collector
	 * if the relay thread is stopped. In case the relay thread is
	 * still running, it may need to access replica->gc so leave the
	 * job to replica_on_relay_stop, which will be called as soon as
	 * the relay thread exits.
	 */
	if (replica->gc != NULL &&
	    relay_get_state(replica->relay) != RELAY_FOLLOW) {
		gc_consumer_unregister(replica->gc);
		replica->gc = NULL;
	}
	if (replica_is_orphan(replica)) {
		replica_hash_remove(&replicaset.hash, replica);
		replica_delete(replica);
	}
}

static void
replica_set_applier(struct replica *replica, struct applier *applier)
{
	assert(replica->applier == NULL);
	replica->applier = applier;
	trigger_add(&replica->applier->on_state,
		    &replica->on_applier_state);
}

static void
replica_clear_applier(struct replica *replica)
{
	assert(replica->applier != NULL);
	replica->applier = NULL;
	trigger_clear(&replica->on_applier_state);
}

static void
replica_on_applier_sync(struct replica *replica)
{
	assert(replica->applier_sync_state == APPLIER_CONNECTED);

	replica->applier_sync_state = APPLIER_SYNC;
	replicaset.applier.synced++;

	replicaset_check_quorum();
}

static void
replica_on_applier_connect(struct replica *replica)
{
	struct applier *applier = replica->applier;

	assert(tt_uuid_is_nil(&replica->uuid));
	assert(!tt_uuid_is_nil(&applier->uuid));
	assert(replica->applier_sync_state == APPLIER_DISCONNECTED);

	replica->uuid = applier->uuid;
	replica->applier_sync_state = APPLIER_CONNECTED;
	replicaset.applier.connected++;

	struct replica *orig = replica_hash_search(&replicaset.hash, replica);
	if (orig != NULL && orig->applier != NULL) {
		say_error("duplicate connection to the same replica: "
			  "instance uuid %s, addr1 %s, addr2 %s",
			  tt_uuid_str(&orig->uuid), applier->source,
			  orig->applier->source);
		fiber_cancel(fiber());
		/*
		 * Raise an exception to force the applier
		 * to disconnect.
		 */
		fiber_testcancel();
	}

	rlist_del_entry(replica, in_anon);

	if (orig != NULL) {
		/* Use existing struct replica */
		assert(orig->applier_sync_state == APPLIER_DISCONNECTED);
		orig->applier_sync_state = replica->applier_sync_state;
		replica_set_applier(orig, applier);
		replica_clear_applier(replica);
		replica_delete(replica);
		replica = orig;
	} else {
		/* Add a new struct replica */
		replica_hash_insert(&replicaset.hash, replica);
	}
}

static void
replica_on_applier_reconnect(struct replica *replica)
{
	struct applier *applier = replica->applier;

	assert(!tt_uuid_is_nil(&replica->uuid));
	assert(!tt_uuid_is_nil(&applier->uuid));
	assert(replica->applier_sync_state == APPLIER_LOADING ||
	       replica->applier_sync_state == APPLIER_DISCONNECTED);

	if (replica->applier_sync_state == APPLIER_LOADING) {
		assert(replicaset.applier.loading > 0);
		replicaset.applier.loading--;
	}

	if (!tt_uuid_is_equal(&replica->uuid, &applier->uuid)) {
		/*
		 * Master's UUID changed, most likely because it was
		 * rebootstrapped. Try to look up a replica matching
		 * the new UUID and reassign the applier to it.
		 */
		struct replica *orig = replica_by_uuid(&applier->uuid);
		if (orig == NULL) {
			orig = replica_new();
			orig->uuid = applier->uuid;
			replica_hash_insert(&replicaset.hash, orig);
		}

		if (orig->applier != NULL) {
			tnt_raise(ClientError, ER_CFG, "replication",
				  "duplicate connection to the same replica");
		}

		replica_set_applier(orig, applier);
		replica_clear_applier(replica);
		replica->applier_sync_state = APPLIER_DISCONNECTED;
		replica = orig;
	}

	replica->applier_sync_state = APPLIER_CONNECTED;
	replicaset.applier.connected++;
}

static void
replica_on_applier_disconnect(struct replica *replica)
{
	switch (replica->applier_sync_state) {
	case APPLIER_SYNC:
		assert(replicaset.applier.synced > 0);
		replicaset.applier.synced--;
		FALLTHROUGH;
	case APPLIER_CONNECTED:
		assert(replicaset.applier.connected > 0);
		replicaset.applier.connected--;
		break;
	case APPLIER_LOADING:
		assert(replicaset.applier.loading > 0);
		replicaset.applier.loading--;
		break;
	case APPLIER_DISCONNECTED:
		break;
	default:
		unreachable();
	}
	replica->applier_sync_state = replica->applier->state;
	if (replica->applier_sync_state == APPLIER_LOADING)
		replicaset.applier.loading++;
}

static int
replica_on_applier_state_f(struct trigger *trigger, void *event)
{
	(void)event;
	struct replica *replica = container_of(trigger,
			struct replica, on_applier_state);
	switch (replica->applier->state) {
	case APPLIER_INITIAL_JOIN:
		replicaset.is_joining = true;
		break;
	case APPLIER_JOINED:
		replicaset.is_joining = false;
		break;
	case APPLIER_CONNECTED:
		try {
			if (tt_uuid_is_nil(&replica->uuid))
				replica_on_applier_connect(replica);
			else
				replica_on_applier_reconnect(replica);
		} catch (Exception *e) {
			return -1;
		}
		break;
	case APPLIER_LOADING:
	case APPLIER_DISCONNECTED:
		replica_on_applier_disconnect(replica);
		break;
	case APPLIER_FOLLOW:
		replica_on_applier_sync(replica);
		break;
	case APPLIER_OFF:
		/*
		 * Connection to self, duplicate connection
		 * to the same master, or the applier fiber
		 * has been cancelled. Assume synced.
		 */
		replica_on_applier_sync(replica);
		break;
	case APPLIER_STOPPED:
		/* Unrecoverable error. */
		replica_on_applier_disconnect(replica);
		break;
	default:
		break;
	}
	fiber_cond_signal(&replicaset.applier.cond);
	return 0;
}

/**
 * Update the replica set with new "applier" objects
 * upon reconfiguration of box.cfg.replication.
 */
static void
replicaset_update(struct applier **appliers, int count)
{
	replica_hash_t uniq;
	memset(&uniq, 0, sizeof(uniq));
	replica_hash_new(&uniq);
	RLIST_HEAD(anon_replicas);
	struct replica *replica, *next;
	struct applier *applier;

	auto uniq_guard = make_scoped_guard([&]{
		replica_hash_foreach_safe(&uniq, replica, next) {
			replica_hash_remove(&uniq, replica);
			replica_clear_applier(replica);
			replica_delete(replica);
		}
	});

	/* Check for duplicate UUID */
	for (int i = 0; i < count; i++) {
		applier = appliers[i];
		replica = replica_new();
		replica_set_applier(replica, applier);

		if (applier->state != APPLIER_CONNECTED) {
			/*
			 * The replica has not received its UUID from
			 * the master yet and thus cannot be added to
			 * the replica set. Instead, add it to the list
			 * of anonymous replicas and setup a trigger
			 * that will insert it into the replica set
			 * when it is finally connected.
			 */
			rlist_add_entry(&anon_replicas, replica, in_anon);
			continue;
		}

		assert(!tt_uuid_is_nil(&applier->uuid));
		replica->uuid = applier->uuid;

		if (replica_hash_search(&uniq, replica) != NULL) {
			replica_clear_applier(replica);
			replica_delete(replica);
			tnt_raise(ClientError, ER_CFG, "replication",
				  "duplicate connection to the same replica");
		}
		replica_hash_insert(&uniq, replica);
	}

	/*
	 * All invariants and conditions are checked, now it is safe to
	 * apply the new configuration. Nothing can fail after this point.
	 */

	/* Prune old appliers */
	replicaset_foreach(replica) {
		if (replica->applier == NULL)
			continue;
		applier = replica->applier;
		replica_clear_applier(replica);
		replica->applier_sync_state = APPLIER_DISCONNECTED;
		applier_stop(applier);
		applier_delete(applier);
	}
	rlist_foreach_entry_safe(replica, &replicaset.anon, in_anon, next) {
		applier = replica->applier;
		replica_clear_applier(replica);
		replica_delete(replica);
		applier_stop(applier);
		applier_delete(applier);
	}
	rlist_create(&replicaset.anon);

	/* Save new appliers */
	replicaset.applier.total = count;
	replicaset.applier.connected = 0;
	replicaset.applier.loading = 0;
	replicaset.applier.synced = 0;

	replica_hash_foreach_safe(&uniq, replica, next) {
		replica_hash_remove(&uniq, replica);

		struct replica *orig = replica_hash_search(&replicaset.hash,
							   replica);
		if (orig != NULL) {
			/* Use existing struct replica */
			replica_set_applier(orig, replica->applier);
			replica_clear_applier(replica);
			replica_delete(replica);
			replica = orig;
		} else {
			/* Add a new struct replica */
			replica_hash_insert(&replicaset.hash, replica);
		}

		replica->applier_sync_state = APPLIER_CONNECTED;
		replicaset.applier.connected++;
	}
	rlist_swap(&replicaset.anon, &anon_replicas);

	assert(replica_hash_first(&uniq) == NULL);
	replica_hash_foreach_safe(&replicaset.hash, replica, next) {
		if (replica_is_orphan(replica)) {
			replica_hash_remove(&replicaset.hash, replica);
			replica_delete(replica);
		}
	}
}

/**
 * Replica set configuration state, shared among appliers.
 */
struct replicaset_connect_state {
	/** Number of successfully connected appliers. */
	int connected;
	/** Number of appliers that failed to connect. */
	int failed;
	/** Signaled when an applier connects or stops. */
	struct fiber_cond wakeup;
};

struct applier_on_connect {
	struct trigger base;
	struct replicaset_connect_state *state;
};

static int
applier_on_connect_f(struct trigger *trigger, void *event)
{
	struct applier_on_connect *on_connect = container_of(trigger,
					struct applier_on_connect, base);
	struct replicaset_connect_state *state = on_connect->state;
	struct applier *applier = (struct applier *)event;

	switch (applier->state) {
	case APPLIER_OFF:
	case APPLIER_STOPPED:
		state->failed++;
		break;
	case APPLIER_CONNECTED:
		state->connected++;
		break;
	default:
		return 0;
	}
	fiber_cond_signal(&state->wakeup);
	applier_pause(applier);
	return 0;
}

void
replicaset_connect(struct applier **appliers, int count,
		   bool connect_quorum)
{
	if (count == 0) {
		/* Cleanup the replica set. */
		replicaset_update(appliers, count);
		return;
	}

	say_info("connecting to %d replicas", count);

	if (!connect_quorum) {
		/*
		 * Enter orphan mode on configuration change and
		 * only leave it when we manage to sync with
		 * replicaset_quorum instances. Don't change
		 * title though, it should be 'loading' during
		 * local recovery.
		 */
		box_do_set_orphan(true);
	}

	/*
	 * Simultaneously connect to remote peers to receive their UUIDs
	 * and fill the resulting set:
	 *
	 * - create a single control channel;
	 * - register a trigger in each applier to wake up our
	 *   fiber via this channel when the remote peer becomes
	 *   connected and a UUID is received;
	 * - wait up to CONNECT_TIMEOUT seconds for `count` messages;
	 * - on timeout, raise a CFG error, cancel and destroy
	 *   the freshly created appliers (done in a guard);
	 * - an success, unregister the trigger, check the UUID set
	 *   for duplicates, fill the result set, return.
	 */

	/* Memory for on_state triggers registered in appliers */
	struct applier_on_connect triggers[VCLOCK_MAX];

	struct replicaset_connect_state state;
	state.connected = state.failed = 0;
	fiber_cond_create(&state.wakeup);

	double timeout = replication_connect_timeout;
	int quorum = MIN(count, replication_connect_quorum);

	/* Add triggers and start simulations connection to remote peers */
	for (int i = 0; i < count; i++) {
		struct applier *applier = appliers[i];
		struct applier_on_connect *trigger = &triggers[i];
		/* Register a trigger to wake us up when peer is connected */
		trigger_create(&trigger->base, applier_on_connect_f, NULL, NULL);
		trigger->state = &state;
		trigger_add(&applier->on_state, &trigger->base);
		/* Start background connection */
		applier_start(applier);
	}

	while (state.connected < count) {
		/*
		 * After a quorum is reached, it is considered
		 * enough to proceed. Except if a connection is
		 * critical.
		 * Connection *is* critical even with 0 quorum
		 * when the instance starts first time and needs
		 * to choose replicaset UUID, fill _cluster, etc.
		 * If 0 quorum allowed to return immediately even
		 * at first start, then it would be impossible to
		 * bootstrap a replicaset - all nodes would start
		 * immediately and choose different cluster UUIDs.
		 */
		if (state.connected >= quorum && !connect_quorum)
			break;
		double wait_start = ev_monotonic_now(loop());
		if (fiber_cond_wait_timeout(&state.wakeup, timeout) != 0)
			break;
		if (count - state.failed < quorum)
			break;
		timeout -= ev_monotonic_now(loop()) - wait_start;
	}
	if (state.connected < count) {
		say_crit("failed to connect to %d out of %d replicas",
			 count - state.connected, count);
		/* Timeout or connection failure. */
		if (connect_quorum && state.connected < quorum) {
			diag_set(ClientError, ER_CFG, "replication",
				 "failed to connect to one or more replicas");
			goto error;
		}
	} else {
		say_info("connected to %d replicas", state.connected);
	}

	for (int i = 0; i < count; i++) {
		/* Unregister the temporary trigger used to wake us up */
		trigger_clear(&triggers[i].base);
		/*
		 * Stop appliers that failed to connect.
		 * They will be restarted once we proceed
		 * to 'subscribe', see replicaset_follow().
		 */
		struct applier *applier = appliers[i];
		if (applier->state != APPLIER_CONNECTED)
			applier_stop(applier);
	}

	/* Now all the appliers are connected, update the replica set. */
	try {
		replicaset_update(appliers, count);
	} catch (Exception *e) {
		goto error;
	}
	return;
error:
	/* Destroy appliers */
	for (int i = 0; i < count; i++) {
		trigger_clear(&triggers[i].base);
		applier_stop(appliers[i]);
	}
	diag_raise();
}

bool
replicaset_needs_rejoin(struct replica **master)
{
	struct replica *leader = NULL;
	replicaset_foreach(replica) {
		struct applier *applier = replica->applier;
		if (applier == NULL)
			continue;

		const struct ballot *ballot = &applier->ballot;
		if (vclock_compare(&ballot->gc_vclock,
				   &replicaset.vclock) <= 0) {
			/*
			 * There's at least one master that still stores
			 * WALs needed by this instance. Proceed to local
			 * recovery.
			 */
			return false;
		}

		const char *uuid_str = tt_uuid_str(&replica->uuid);
		const char *addr_str = sio_strfaddr(&applier->addr,
						applier->addr_len);
		const char *local_vclock_str = vclock_to_string(&replicaset.vclock);
		const char *remote_vclock_str = vclock_to_string(&ballot->vclock);
		const char *gc_vclock_str = vclock_to_string(&ballot->gc_vclock);

		say_info("can't follow %s at %s: required %s available %s",
			 uuid_str, addr_str, local_vclock_str, gc_vclock_str);

		if (vclock_compare(&replicaset.vclock, &ballot->vclock) > 0) {
			/*
			 * Replica has some rows that are not present on
			 * the master. Don't rebootstrap as we don't want
			 * to lose any data.
			 */
			say_info("can't rebootstrap from %s at %s: "
				 "replica has local rows: local %s remote %s",
				 uuid_str, addr_str, local_vclock_str,
				 remote_vclock_str);
			continue;
		}

		/* Prefer a master with the max vclock. */
		if (leader == NULL ||
		    vclock_sum(&ballot->vclock) >
		    vclock_sum(&leader->applier->ballot.vclock))
			leader = replica;
	}
	if (leader == NULL)
		return false;

	*master = leader;
	return true;
}

void
replicaset_follow(void)
{
	struct replica *replica;
	replicaset_foreach(replica) {
		/* Resume connected appliers. */
		if (replica->applier != NULL)
			applier_resume(replica->applier);
	}
	rlist_foreach_entry(replica, &replicaset.anon, in_anon) {
		/* Restart appliers that failed to connect. */
		applier_start(replica->applier);
	}
}

void
replicaset_sync(void)
{
	int quorum = replicaset_quorum();

	if (quorum == 0) {
		/*
		 * Quorum is 0 or replication is not configured.
		 * Leaving 'orphan' state immediately.
		 */
		box_set_orphan(false);
		return;
	}

	say_info("synchronizing with %d replicas", quorum);

	/*
	 * Wait until all connected replicas synchronize up to
	 * replication_sync_lag or return on replication_sync_timeout
	 */
	double deadline = ev_monotonic_now(loop()) + replication_sync_timeout;
	while (replicaset.applier.synced < quorum &&
	       replicaset.applier.connected +
	       replicaset.applier.loading >= quorum) {
		if (fiber_cond_wait_deadline(&replicaset.applier.cond,
					     deadline) != 0)
			break;
	}

	if (replicaset.applier.synced < quorum) {
		/*
		 * Not enough replicas connected to form a quorum.
		 * Do not stall configuration, leave the instance
		 * in 'orphan' state.
		 */
		say_crit("failed to synchronize with %d out of %d replicas",
			 replicaset.applier.total - replicaset.applier.synced,
			 replicaset.applier.total);
		box_set_orphan(true);
	} else {
		say_info("replica set sync complete");
		box_set_orphan(false);
	}
}

void
replicaset_check_quorum(void)
{
	if (replicaset.applier.synced >= replicaset_quorum())
		box_set_orphan(false);
}

void
replica_on_relay_stop(struct replica *replica)
{
	/*
	 * If the replica was evicted from the cluster, we don't
	 * need to keep WALs for it anymore. Unregister it with
	 * the garbage collector then. See also replica_clear_id.
	 */
	assert(replica->gc != NULL);
	if (replica->id == REPLICA_ID_NIL) {
		gc_consumer_unregister(replica->gc);
		replica->gc = NULL;
	}
	if (replica_is_orphan(replica)) {
		replica_hash_remove(&replicaset.hash, replica);
		replica_delete(replica);
	}
}

struct replica *
replicaset_first(void)
{
	return replica_hash_first(&replicaset.hash);
}

struct replica *
replicaset_next(struct replica *replica)
{
	return replica_hash_next(&replicaset.hash, replica);
}

/**
 * Compare vclock, read only mode and orphan status
 * of all connected replicas and elect a leader.
 * Initiallly, skip read-only replicas, since they
 * can not properly act as bootstrap masters (register
 * new nodes in _cluster table). If there are no read-write
 * replicas, choose a read-only replica with biggest vclock
 * as a leader, in hope it will become read-write soon.
 */
static struct replica *
replicaset_round(bool skip_ro)
{
	struct replica *leader = NULL;
	replicaset_foreach(replica) {
		struct applier *applier = replica->applier;
		if (applier == NULL)
			continue;
		/**
		 * While bootstrapping a new cluster, read-only
		 * replicas shouldn't be considered as a leader.
		 * The only exception if there is no read-write
		 * replicas since there is still a possibility
		 * that all replicas exist in cluster table.
		 */
		if (skip_ro && applier->ballot.is_ro)
			continue;
		if (leader == NULL) {
			leader = replica;
			continue;
		}
		/*
		 * Try to find a replica which has already left
		 * orphan mode.
		 */
		if (applier->ballot.is_loading && ! leader->applier->ballot.is_loading)
			continue;
		/*
		 * Choose the replica with the most advanced
		 * vclock. If there are two or more replicas
		 * with the same vclock, prefer the one with
		 * the lowest uuid.
		 */
		int cmp = vclock_compare(&applier->ballot.vclock,
				&leader->applier->ballot.vclock);
		if (cmp < 0)
			continue;
		if (cmp == 0 && tt_uuid_compare(&replica->uuid,
						&leader->uuid) > 0)
			continue;
		leader = replica;
	}
	return leader;
}

struct replica *
replicaset_leader(void)
{
	bool skip_ro = true;
	/**
	 * Two loops, first prefers read-write replicas among others.
	 * Second for backward compatibility, if there is no such
	 * replicas at all.
	 */
	struct replica *leader = replicaset_round(skip_ro);
	if (leader == NULL) {
		skip_ro = false;
		leader = replicaset_round(skip_ro);
	}

	return leader;
}

struct replica *
replica_by_uuid(const struct tt_uuid *uuid)
{
	struct replica key;
	key.uuid = *uuid;
	return replica_hash_search(&replicaset.hash, &key);
}

struct replica *
replica_by_id(uint32_t replica_id)
{
	return replicaset.replica_by_id[replica_id];
}
