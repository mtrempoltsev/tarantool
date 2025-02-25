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
#include "fiber.h"

#include <trivia/config.h>
#include <trivia/util.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pmatomic.h>

#include "assoc.h"
#include "memory.h"
#include "trigger.h"

#include "third_party/valgrind/memcheck.h"

static int (*fiber_invoke)(fiber_func f, va_list ap);

#if ENABLE_ASAN
#include <sanitizer/asan_interface.h>

#define ASAN_START_SWITCH_FIBER(var_name, will_switch_back, bottom, size) \
	void *var_name = NULL; \
	__sanitizer_start_switch_fiber((will_switch_back) ? &var_name : NULL, \
                                       (bottom), (size))
#if ASAN_INTERFACE_OLD
#define ASAN_FINISH_SWITCH_FIBER(var_name) \
	__sanitizer_finish_switch_fiber(var_name);
#else
#define ASAN_FINISH_SWITCH_FIBER(var_name) \
	__sanitizer_finish_switch_fiber(var_name, 0, 0);
#endif

#else
#define ASAN_START_SWITCH_FIBER(var_name, will_switch_back, bottom, size)
#define ASAN_FINISH_SWITCH_FIBER(var_name)
#endif

#define fiber_madvise(addr, len, advice)				\
({									\
	int err = madvise((addr), (len), (advice));			\
	if (err)							\
		say_syserror("madvise");				\
	err;								\
})

#define fiber_mprotect(addr, len, prot)					\
({									\
	int err = mprotect((addr), (len), (prot));			\
	if (err)							\
		say_syserror("mprotect");				\
	err;								\
})

/*
 * Defines a handler to be executed on exit from cord's thread func,
 * accessible via cord()->on_exit (normally NULL). It is used to
 * implement cord_cojoin.
 */
struct cord_on_exit {
	void (*callback)(void*);
	void *argument;
};

/*
 * A special value distinct from any valid pointer to cord_on_exit
 * structure AND NULL. This value is stored in cord()->on_exit by the
 * thread function prior to thread termination.
 */
static const struct cord_on_exit cord_on_exit_sentinel = { NULL, NULL };
#define CORD_ON_EXIT_WONT_RUN (&cord_on_exit_sentinel)

static struct cord main_cord;
__thread struct cord *cord_ptr = NULL;
pthread_t main_thread_id;

static size_t page_size;
static int stack_direction;

enum {
	/* The minimum allowable fiber stack size in bytes */
	FIBER_STACK_SIZE_MINIMAL = 16384,
	/* Default fiber stack size in bytes */
	FIBER_STACK_SIZE_DEFAULT = 524288,
	/* Stack size watermark in bytes. */
	FIBER_STACK_SIZE_WATERMARK = 65536,
};

/** Default fiber attributes */
static const struct fiber_attr fiber_attr_default = {
       .stack_size = FIBER_STACK_SIZE_DEFAULT,
       .flags = FIBER_DEFAULT_FLAGS
};

#ifdef HAVE_MADV_DONTNEED
/*
 * Random values generated with uuid.
 * Used for stack poisoning.
 */
static const uint64_t poison_pool[] = {
	0x74f31d37285c4c37, 0xb10269a05bf10c29,
	0x0994d845bd284e0f, 0x9ffd4f7129c184df,
	0x357151e6711c4415, 0x8c5e5f41aafe6f28,
	0x6917dd79e78049d5, 0xba61957c65ca2465,
};

/*
 * We poison by 8 bytes as it's natural for stack
 * step on x86-64. Also 128 byte gap between poison
 * values should cover common cases.
 */
#define POISON_SIZE	(sizeof(poison_pool) / sizeof(poison_pool[0]))
#define POISON_OFF	(128 / sizeof(poison_pool[0]))

#endif /* HAVE_MADV_DONTNEED */

void
fiber_attr_create(struct fiber_attr *fiber_attr)
{
       *fiber_attr = fiber_attr_default;
}

struct fiber_attr *
fiber_attr_new()
{
	struct fiber_attr *fiber_attr = malloc(sizeof(*fiber_attr));
	if (fiber_attr == NULL)  {
		diag_set(OutOfMemory, sizeof(*fiber_attr),
			 "runtime", "fiber attr");
		return NULL;
	}
	fiber_attr_create(fiber_attr);
	return fiber_attr;
}

void
fiber_attr_delete(struct fiber_attr *fiber_attr)
{
	free(fiber_attr);
}

int
fiber_attr_setstacksize(struct fiber_attr *fiber_attr, size_t stack_size)
{
	if (stack_size < FIBER_STACK_SIZE_MINIMAL) {
		errno = EINVAL;
		diag_set(SystemError, "stack size is too small");
		return -1;
	}
	fiber_attr->stack_size = stack_size;
	if (stack_size != FIBER_STACK_SIZE_DEFAULT) {
		fiber_attr->flags |= FIBER_CUSTOM_STACK;
	} else {
		fiber_attr->flags &= ~FIBER_CUSTOM_STACK;
	}
	return 0;
}

size_t
fiber_attr_getstacksize(struct fiber_attr *fiber_attr)
{
	return fiber_attr != NULL ? fiber_attr->stack_size :
				    fiber_attr_default.stack_size;
}

static void
fiber_recycle(struct fiber *fiber);

static void
fiber_stack_recycle(struct fiber *fiber);

static void
fiber_destroy(struct cord *cord, struct fiber *f);

/**
 * Transfer control to callee fiber.
 */
static void
fiber_call_impl(struct fiber *callee)
{
	struct fiber *caller = fiber();
	struct cord *cord = cord();

	/* Ensure we aren't switching to a fiber parked in fiber_loop */
	assert(callee->f != NULL && callee->fid != 0);
	assert(callee->flags & FIBER_IS_READY || callee == &cord->sched);
	assert(! (callee->flags & FIBER_IS_DEAD));
	/*
	 * Ensure the callee was removed from cord->ready list.
	 * If it wasn't, the callee will observe a 'spurious' wakeup
	 * later, due to a fiber_wakeup() performed in the past.
	 */
	assert(rlist_empty(&callee->state));
	assert(caller);
	assert(caller != callee);

	cord->fiber = callee;

	callee->flags &= ~FIBER_IS_READY;
	callee->csw++;
	ASAN_START_SWITCH_FIBER(asan_state, 1,
				callee->stack,
				callee->stack_size);
	coro_transfer(&caller->ctx, &callee->ctx);
	ASAN_FINISH_SWITCH_FIBER(asan_state);
}

void
fiber_call(struct fiber *callee)
{
	struct fiber *caller = fiber();
	assert(! (caller->flags & FIBER_IS_READY));
	assert(rlist_empty(&callee->state));
	assert(! (callee->flags & FIBER_IS_READY));

	/** By convention, these triggers must not throw. */
	if (! rlist_empty(&caller->on_yield))
		trigger_run(&caller->on_yield, NULL);
	callee->caller = caller;
	callee->flags |= FIBER_IS_READY;
	caller->flags |= FIBER_IS_READY;
	fiber_call_impl(callee);
}

void
fiber_start(struct fiber *callee, ...)
{
	va_start(callee->f_data, callee);
	fiber_call(callee);
	va_end(callee->f_data);
}

bool
fiber_checkstack()
{
	return false;
}

/**
 * Interrupt a synchronous wait of a fiber inside the event loop.
 * We do so by keeping an "async" event in every fiber, solely
 * for this purpose, and raising this event here.
 *
 * @note: if this is sent to self, followed by a fiber_yield()
 * call, it simply reschedules the fiber after other ready
 * fibers in the same event loop iteration.
 */
void
fiber_wakeup(struct fiber *f)
{
	assert(! (f->flags & FIBER_IS_DEAD));
	/**
	 * Do nothing if the fiber is already in cord->ready
	 * list *or* is in the call chain created by
	 * fiber_schedule_list(). While it's harmless to re-add
	 * a fiber to cord->ready, even if it's already there,
	 * but the same game is deadly when the fiber is in
	 * the callee list created by fiber_schedule_list().
	 *
	 * To put it another way, fiber_wakeup() is a 'request' to
	 * schedule the fiber for execution, and once it is executing
	 * a wakeup request is considered complete and it must be
	 * removed.
	 *
	 * A dead fiber can be lingering in the cord fiber list
	 * if it is joinable. This makes it technically possible
	 * to schedule it. We would never make such a mistake
	 * in our own code, hence the assert above. But as long
	 * as fiber.wakeup() is a part of public Lua API, an
	 * external rock can mess things up. Ignore such attempts
	 * as well.
	 */
	if (f->flags & (FIBER_IS_READY | FIBER_IS_DEAD))
		return;
	struct cord *cord = cord();
	if (rlist_empty(&cord->ready)) {
		/*
		 * ev_feed_event(EV_CUSTOM) gets scheduled in the
		 * same event loop iteration, and we rely on this
		 * for quick scheduling. For a wakeup which
		 * actually can invoke a poll() in libev,
		 * use fiber_sleep(0)
		 */
		ev_feed_event(cord->loop, &cord->wakeup_event, EV_CUSTOM);
	}
	/**
	 * Removes the fiber from whatever wait list it is on.
	 *
	 * It's critical that the newly scheduled fiber is
	 * added to the tail of the list, to preserve correct
	 * transaction commit order after a successful WAL write.
	 * (see tx_schedule_commit()/tx_schedule_rollback() in
	 * box/wal.cc)
	 */
	rlist_move_tail_entry(&cord->ready, f, state);
	f->flags |= FIBER_IS_READY;
}

/** Cancel the subject fiber.
*
 * Note: cancelation is asynchronous. Use fiber_join() to wait for the
 * cancelation to complete.
 *
 * A fiber may opt to set FIBER_IS_CANCELLABLE to false, and never test
 * that it was cancelled.  Such fiber can not ever be cancelled.
 * However, as long as most of the cooperative code calls
 * fiber_testcancel(), most of the fibers are cancellable.
 *
 * The fiber which is cancelled, has FiberIsCancelled raised
 * in it. For cancellation to work, this exception type should be
 * re-raised whenever (if) it is caught.
 */
void
fiber_cancel(struct fiber *f)
{
	assert(f->fid != 0);
	struct fiber *self = fiber();
	/**
	 * Do nothing if the fiber is dead, since cancelling
	 * the fiber would clear the diagnostics area and
	 * the cause of death would be lost.
	 */
	if (fiber_is_dead(f))
		return;

	f->flags |= FIBER_IS_CANCELLED;

	/**
	 * Don't wake self and zombies.
	 */
	if (f != self) {
		if (f->flags & FIBER_IS_CANCELLABLE)
			fiber_wakeup(f);
	}
}

/**
 * Change the current cancellation state of a fiber. This is not
 * a cancellation point.
 */
bool
fiber_set_cancellable(bool yesno)
{
	bool prev = fiber()->flags & FIBER_IS_CANCELLABLE;
	if (yesno == true)
		fiber()->flags |= FIBER_IS_CANCELLABLE;
	else
		fiber()->flags &= ~FIBER_IS_CANCELLABLE;
	return prev;
}

bool
fiber_is_cancelled()
{
	return fiber()->flags & FIBER_IS_CANCELLED;
}

void
fiber_set_joinable(struct fiber *fiber, bool yesno)
{
	if (yesno == true)
		fiber->flags |= FIBER_IS_JOINABLE;
	else
		fiber->flags &= ~FIBER_IS_JOINABLE;
}

/** Report libev time (cheap). */
double
fiber_time(void)
{
	return ev_now(loop());
}

uint64_t
fiber_time64(void)
{
	return (uint64_t) ( ev_now(loop()) * 1000000 + 0.5 );
}

double
fiber_clock(void)
{
	return ev_monotonic_now(loop());
}

uint64_t
fiber_clock64(void)
{
	return (uint64_t) ( ev_monotonic_now(loop()) * 1000000 + 0.5 );
}

/**
 * Move current fiber to the end of ready fibers list and switch to next
 */
void
fiber_reschedule(void)
{
	fiber_wakeup(fiber());
	fiber_yield();
}

int
fiber_join(struct fiber *fiber)
{
	assert(fiber->flags & FIBER_IS_JOINABLE);

	if (! fiber_is_dead(fiber)) {
		do {
			/*
			 * In case fiber is cancelled during yield
			 * it will be removed from wake queue by a
			 * wakeup following the cancel, so we have
			 * to put it back in.
			 */
			rlist_add_tail_entry(&fiber->wake, fiber(), state);
			fiber_yield();
		} while (! fiber_is_dead(fiber));
	}

	/* Move exception to the caller */
	int ret = fiber->f_ret;
	if (ret != 0) {
		assert(!diag_is_empty(&fiber->diag));
		diag_move(&fiber->diag, &fiber()->diag);
	}
	/* The fiber is already dead. */
	fiber_recycle(fiber);
	return ret;
}

/**
 * @note: this is not a cancellation point (@sa fiber_testcancel())
 * but it is considered good practice to call testcancel()
 * after each yield.
 */
void
fiber_yield(void)
{
	struct cord *cord = cord();
	struct fiber *caller = cord->fiber;
	struct fiber *callee = caller->caller;
	caller->caller = &cord->sched;

	/** By convention, these triggers must not throw. */
	if (! rlist_empty(&caller->on_yield))
		trigger_run(&caller->on_yield, NULL);

	assert(callee->flags & FIBER_IS_READY || callee == &cord->sched);
	assert(! (callee->flags & FIBER_IS_DEAD));
	cord->fiber = callee;
	callee->csw++;
	callee->flags &= ~FIBER_IS_READY;
	ASAN_START_SWITCH_FIBER(asan_state,
				(caller->flags & FIBER_IS_DEAD) == 0,
				callee->stack,
				callee->stack_size);
	coro_transfer(&caller->ctx, &callee->ctx);
	ASAN_FINISH_SWITCH_FIBER(asan_state);
}

struct fiber_watcher_data {
	struct fiber *f;
	bool timed_out;
};

static void
fiber_schedule_timeout(ev_loop *loop,
		       ev_timer *watcher, int revents)
{
	(void) loop;
	(void) revents;

	assert(fiber() == &cord()->sched);
	struct fiber_watcher_data *state =
			(struct fiber_watcher_data *) watcher->data;
	state->timed_out = true;
	fiber_wakeup(state->f);
}

/**
 * @brief yield & check timeout
 * @return true if timeout exceeded
 */
bool
fiber_yield_timeout(ev_tstamp delay)
{
	struct ev_timer timer;
	ev_timer_init(&timer, fiber_schedule_timeout, delay, 0);
	struct fiber_watcher_data state = { fiber(), false };
	timer.data = &state;
	ev_timer_start(loop(), &timer);
	fiber_yield();
	ev_timer_stop(loop(), &timer);
	return state.timed_out;
}

/**
 * Yield the current fiber to events in the event loop.
 */
void
fiber_sleep(double delay)
{
	/*
	 * libev sleeps at least backend_mintime, which is 1 ms in
	 * case of poll()/Linux, unless there are idle watchers.
	 * So, to properly implement fiber_sleep(0), i.e. a sleep
	 * with a zero timeout, we set up an idle watcher, and
	 * it triggers libev to poll() with zero timeout.
	 */
	if (delay == 0) {
		ev_idle_start(loop(), &cord()->idle_event);
	}
	/*
	 * We don't use fiber_wakeup() here to ensure there is
	 * no infinite wakeup loop in case of fiber_sleep(0).
	 */
	fiber_yield_timeout(delay);

	if (delay == 0) {
		ev_idle_stop(loop(), &cord()->idle_event);
	}
}

void
fiber_schedule_cb(ev_loop *loop, ev_watcher *watcher, int revents)
{
	(void) loop;
	(void) revents;
	struct fiber *fiber = watcher->data;
	assert(fiber() == &cord()->sched);
	fiber_wakeup(fiber);
}

static inline void
fiber_schedule_list(struct rlist *list)
{
	struct fiber *first;
	struct fiber *last;

	/*
	 * Happens when a fiber exits and is removed from cord->ready
	 * resulting in the empty list.
	 */
	if (rlist_empty(list))
		return;

	first = last = rlist_shift_entry(list, struct fiber, state);
	assert(last->flags & FIBER_IS_READY);

	while (! rlist_empty(list)) {
		last->caller = rlist_shift_entry(list, struct fiber, state);
		last = last->caller;
		assert(last->flags & FIBER_IS_READY);
	}
	last->caller = fiber();
	assert(fiber() == &cord()->sched);
	fiber_call_impl(first);
}

static void
fiber_schedule_wakeup(ev_loop *loop, ev_async *watcher, int revents)
{
	(void) loop;
	(void) watcher;
	(void) revents;
	struct cord *cord = cord();
	fiber_schedule_list(&cord->ready);
}

static void
fiber_schedule_idle(ev_loop *loop, ev_idle *watcher,
		    int revents)
{
	(void) loop;
	(void) watcher;
	(void) revents;
}


struct fiber *
fiber_find(uint32_t fid)
{
	struct mh_i32ptr_t *fiber_registry = cord()->fiber_registry;
	mh_int_t k = mh_i32ptr_find(fiber_registry, fid, NULL);

	if (k == mh_end(fiber_registry))
		return NULL;
	return (struct fiber *) mh_i32ptr_node(fiber_registry, k)->val;
}

static void
register_fid(struct fiber *fiber)
{
	struct mh_i32ptr_node_t node = { fiber->fid, fiber };
	mh_i32ptr_put(cord()->fiber_registry, &node, NULL, NULL);
}

static void
unregister_fid(struct fiber *fiber)
{
	struct mh_i32ptr_node_t node = { fiber->fid, NULL };
	mh_i32ptr_remove(cord()->fiber_registry, &node, NULL);
}

struct fiber *
fiber_self()
{
	return fiber();
}

void
fiber_gc(void)
{
	if (region_used(&fiber()->gc) < 128 * 1024) {
		region_reset(&fiber()->gc);
		return;
	}

	region_free(&fiber()->gc);
}

/** Common part of fiber_new() and fiber_recycle(). */
static void
fiber_reset(struct fiber *fiber)
{
	rlist_create(&fiber->on_yield);
	rlist_create(&fiber->on_stop);
	fiber->flags = FIBER_DEFAULT_FLAGS;
}

/** Destroy an active fiber and prepare it for reuse. */
static void
fiber_recycle(struct fiber *fiber)
{
	/* no exceptions are leaking */
	assert(diag_is_empty(&fiber->diag));
	/* no pending wakeup */
	assert(rlist_empty(&fiber->state));
	bool has_custom_stack = fiber->flags & FIBER_CUSTOM_STACK;
	fiber_stack_recycle(fiber);
	fiber_reset(fiber);
	fiber->name[0] = '\0';
	fiber->f = NULL;
	fiber->wait_pad = NULL;
	memset(&fiber->storage, 0, sizeof(fiber->storage));
	unregister_fid(fiber);
	fiber->fid = 0;
	region_free(&fiber->gc);
	if (!has_custom_stack) {
		rlist_move_entry(&cord()->dead, fiber, link);
	} else {
		fiber_destroy(cord(), fiber);
	}
}

static void
fiber_loop(MAYBE_UNUSED void *data)
{
	ASAN_FINISH_SWITCH_FIBER(NULL);
	for (;;) {
		struct fiber *fiber = fiber();

		assert(fiber != NULL && fiber->f != NULL && fiber->fid != 0);
		fiber->f_ret = fiber_invoke(fiber->f, fiber->f_data);
		if (fiber->f_ret != 0) {
			struct error *e = diag_last_error(&fiber->diag);
			/* diag must not be empty on error */
			assert(e != NULL || fiber->flags & FIBER_IS_CANCELLED);
			/*
			 * For joinable fibers, it's the business
			 * of the caller to deal with the error.
			 */
			if (!(fiber->flags & FIBER_IS_JOINABLE)) {
				if (!(fiber->flags & FIBER_IS_CANCELLED))
					error_log(e);
				diag_clear(&fiber()->diag);
			}
		} else {
			/*
			 * Make sure a leftover exception does not
			 * propagate up to the joiner.
			 */
			diag_clear(&fiber()->diag);
		}
		fiber->flags |= FIBER_IS_DEAD;
		while (! rlist_empty(&fiber->wake)) {
		       struct fiber *f;
		       f = rlist_shift_entry(&fiber->wake, struct fiber,
					     state);
		       assert(f != fiber);
		       fiber_wakeup(f);
	        }
		if (! rlist_empty(&fiber->on_stop))
			trigger_run(&fiber->on_stop, fiber);
		/* reset pending wakeups */
		rlist_del(&fiber->state);
		if (! (fiber->flags & FIBER_IS_JOINABLE))
			fiber_recycle(fiber);
		/*
		 * Crash if spurious wakeup happens, don't call the old
		 * function again, ap is garbage by now.
		 */
		fiber->f = NULL;
		fiber_yield();	/* give control back to scheduler */
	}
}

void
fiber_set_name(struct fiber *fiber, const char *name)
{
	assert(name != NULL);
	snprintf(fiber->name, sizeof(fiber->name), "%s", name);
}

static inline void *
page_align_down(void *ptr)
{
	return (void *)((intptr_t)ptr & ~(page_size - 1));
}

static inline void *
page_align_up(void *ptr)
{
	return page_align_down(ptr + page_size - 1);
}

#ifdef HAVE_MADV_DONTNEED
/**
 * Check if stack poison values are present starting from
 * the address provided.
 */
static bool
stack_has_watermark(void *addr)
{
	const uint64_t *src = poison_pool;
	const uint64_t *dst = addr;
	size_t i;

	for (i = 0; i < POISON_SIZE; i++) {
		if (*dst != src[i])
			return false;
		dst += POISON_OFF;
	}
	return true;
}

/**
 * Put stack poison values starting from the address provided.
 */
static void
stack_put_watermark(void *addr)
{
	const uint64_t *src = poison_pool;
	uint64_t *dst = addr;
	size_t i;

	for (i = 0; i < POISON_SIZE; i++) {
		*dst = src[i];
		dst += POISON_OFF;
	}
}

/**
 * Free stack memory above the watermark when a fiber is recycled.
 * To avoid a pointless syscall invocation in case the fiber hasn't
 * touched memory above the watermark, we only call madvise() if
 * the fiber has overwritten a poison value.
 */
static void
fiber_stack_recycle(struct fiber *fiber)
{
	if (fiber->stack_watermark == NULL ||
	    stack_has_watermark(fiber->stack_watermark))
		return;
	/*
	 * When dropping pages make sure the page containing
	 * the watermark isn't touched since we're updating
	 * it anyway.
	 */
	void *start, *end;
	if (stack_direction < 0) {
		start = fiber->stack;
		end = page_align_down(fiber->stack_watermark);
	} else {
		start = page_align_up(fiber->stack_watermark);
		end = fiber->stack + fiber->stack_size;
	}
	fiber_madvise(start, end - start, MADV_DONTNEED);
	stack_put_watermark(fiber->stack_watermark);
}

/**
 * Initialize fiber stack watermark.
 */
static void
fiber_stack_watermark_create(struct fiber *fiber)
{
	assert(fiber->stack_watermark == NULL);

	/* No tracking on custom stacks for simplicity. */
	if (fiber->flags & FIBER_CUSTOM_STACK)
		return;

	/*
	 * We don't expect the whole stack usage in regular
	 * loads, let's try to minimize rss pressure.
	 */
	fiber_madvise(fiber->stack, fiber->stack_size, MADV_DONTNEED);

	/*
	 * To increase probability of stack overflow detection
	 * we put the first mark at a random position.
	 */
	size_t offset = rand() % POISON_OFF * sizeof(poison_pool[0]);
	if (stack_direction < 0) {
		fiber->stack_watermark  = fiber->stack + fiber->stack_size;
		fiber->stack_watermark -= FIBER_STACK_SIZE_WATERMARK;
		fiber->stack_watermark += offset;
	} else {
		fiber->stack_watermark  = fiber->stack;
		fiber->stack_watermark += FIBER_STACK_SIZE_WATERMARK;
		fiber->stack_watermark -= page_size;
		fiber->stack_watermark += offset;
	}
	stack_put_watermark(fiber->stack_watermark);
}
#else
static void
fiber_stack_recycle(struct fiber *fiber)
{
	(void)fiber;
}

static void
fiber_stack_watermark_create(struct fiber *fiber)
{
	(void)fiber;
}
#endif /* HAVE_MADV_DONTNEED */

static void
fiber_stack_destroy(struct fiber *fiber, struct slab_cache *slabc)
{
	if (fiber->stack != NULL) {
		VALGRIND_STACK_DEREGISTER(fiber->stack_id);
#if ENABLE_ASAN
		ASAN_UNPOISON_MEMORY_REGION(fiber->stack, fiber->stack_size);
#endif
		void *guard;
		if (stack_direction < 0)
			guard = page_align_down(fiber->stack - page_size);
		else
			guard = page_align_up(fiber->stack + fiber->stack_size);
		fiber_mprotect(guard, page_size, PROT_READ | PROT_WRITE);
		slab_put(slabc, fiber->stack_slab);
	}
}

static int
fiber_stack_create(struct fiber *fiber, struct slab_cache *slabc,
		   size_t stack_size)
{
	stack_size -= slab_sizeof();
	fiber->stack_slab = slab_get(slabc, stack_size);

	if (fiber->stack_slab == NULL) {
		diag_set(OutOfMemory, stack_size,
			 "runtime arena", "fiber stack");
		return -1;
	}
	void *guard;
	/* Adjust begin and size for stack memory chunk. */
	if (stack_direction < 0) {
		/*
		 * A stack grows down. First page after begin of a
		 * stack memory chunk should be protected and memory
		 * after protected page until end of memory chunk can be
		 * used for coro stack usage.
		 */
		guard = page_align_up(slab_data(fiber->stack_slab));
		fiber->stack = guard + page_size;
		fiber->stack_size = slab_data(fiber->stack_slab) + stack_size -
				    fiber->stack;
	} else {
		/*
		 * A stack grows up. Last page should be protected and
		 * memory from begin of chunk until protected page can
		 * be used for coro stack usage
		 */
		guard = page_align_down(fiber->stack_slab + stack_size) -
			page_size;
		fiber->stack = fiber->stack_slab + slab_sizeof();
		fiber->stack_size = guard - fiber->stack;
	}

	fiber->stack_id = VALGRIND_STACK_REGISTER(fiber->stack,
						  (char *)fiber->stack +
						  fiber->stack_size);

	if (fiber_mprotect(guard, page_size, PROT_NONE)) {
		fiber_stack_destroy(fiber, slabc);
		return -1;
	}

	fiber_stack_watermark_create(fiber);
	return 0;
}

struct fiber *
fiber_new_ex(const char *name, const struct fiber_attr *fiber_attr,
	     fiber_func f)
{
	struct cord *cord = cord();
	struct fiber *fiber = NULL;
	assert(fiber_attr != NULL);

	/* Now we can not reuse fiber if custom attribute was set */
	if (!(fiber_attr->flags & FIBER_CUSTOM_STACK) &&
	    !rlist_empty(&cord->dead)) {
		fiber = rlist_first_entry(&cord->dead,
					  struct fiber, link);
		rlist_move_entry(&cord->alive, fiber, link);
	} else {
		fiber = (struct fiber *)
			mempool_alloc(&cord->fiber_mempool);
		if (fiber == NULL) {
			diag_set(OutOfMemory, sizeof(struct fiber),
				 "fiber pool", "fiber");
			return NULL;
		}
		memset(fiber, 0, sizeof(struct fiber));

		if (fiber_stack_create(fiber, &cord()->slabc,
				       fiber_attr->stack_size)) {
			mempool_free(&cord->fiber_mempool, fiber);
			return NULL;
		}
		coro_create(&fiber->ctx, fiber_loop, NULL,
			    fiber->stack, fiber->stack_size);

		region_create(&fiber->gc, &cord->slabc);

		rlist_create(&fiber->state);
		rlist_create(&fiber->wake);
		diag_create(&fiber->diag);
		fiber_reset(fiber);
		fiber->flags = fiber_attr->flags;

		rlist_add_entry(&cord->alive, fiber, link);
	}

	fiber->f = f;
	/* Excluding reserved range */
	if (++cord->max_fid < FIBER_ID_MAX_RESERVED)
		cord->max_fid = FIBER_ID_MAX_RESERVED + 1;
	fiber->fid = cord->max_fid;
	fiber_set_name(fiber, name);
	register_fid(fiber);

	return fiber;

}

/**
 * Create a new fiber.
 *
 * Takes a fiber from fiber cache, if it's not empty.
 * Can fail only if there is not enough memory for
 * the fiber structure or fiber stack.
 *
 * The created fiber automatically returns itself
 * to the fiber cache when its "main" function
 * completes.
 */
struct fiber *
fiber_new(const char *name, fiber_func f)
{
	return fiber_new_ex(name, &fiber_attr_default, f);
}

/**
 * Free as much memory as possible taken by the fiber.
 *
 * Sic: cord()->sched needs manual destruction in
 * cord_destroy().
 */
static void
fiber_destroy(struct cord *cord, struct fiber *f)
{
	if (f == fiber()) {
		/** End of the application. */
		assert(cord == &main_cord);
		return;
	}
	assert(f != &cord->sched);

	trigger_destroy(&f->on_yield);
	trigger_destroy(&f->on_stop);
	rlist_del(&f->state);
	rlist_del(&f->link);
	region_destroy(&f->gc);
	fiber_stack_destroy(f, &cord->slabc);
	diag_destroy(&f->diag);
}

void
fiber_destroy_all(struct cord *cord)
{
	while (!rlist_empty(&cord->alive))
		fiber_destroy(cord, rlist_first_entry(&cord->alive,
						      struct fiber, link));
	while (!rlist_empty(&cord->dead))
		fiber_destroy(cord, rlist_first_entry(&cord->dead,
						      struct fiber, link));
}

void
cord_create(struct cord *cord, const char *name)
{
	cord() = cord;
	slab_cache_set_thread(&cord()->slabc);

	cord->id = pthread_self();
	cord->on_exit = NULL;
	slab_cache_create(&cord->slabc, &runtime);
	mempool_create(&cord->fiber_mempool, &cord->slabc,
		       sizeof(struct fiber));
	rlist_create(&cord->alive);
	rlist_create(&cord->ready);
	rlist_create(&cord->dead);
	cord->fiber_registry = mh_i32ptr_new();

	/* sched fiber is not present in alive/ready/dead list. */
	cord->sched.fid = FIBER_ID_SCHED;
	fiber_reset(&cord->sched);
	diag_create(&cord->sched.diag);
	region_create(&cord->sched.gc, &cord->slabc);
	fiber_set_name(&cord->sched, "sched");
	cord->fiber = &cord->sched;

	cord->max_fid = FIBER_ID_MAX_RESERVED;
	/*
	 * No need to start this event since it's only used for
	 * ev_feed_event(). Saves a few cycles on every
	 * event loop iteration.
	 */
	ev_async_init(&cord->wakeup_event, fiber_schedule_wakeup);

	ev_idle_init(&cord->idle_event, fiber_schedule_idle);
	cord_set_name(name);

#if ENABLE_ASAN
	/* Record stack extents */
	tt_pthread_attr_getstack(cord->id, &cord->sched.stack,
				 &cord->sched.stack_size);
#else
	cord->sched.stack = NULL;
	cord->sched.stack_size = 0;
#endif

#ifdef HAVE_MADV_DONTNEED
	cord->sched.stack_watermark = NULL;
#endif
}

void
cord_destroy(struct cord *cord)
{
	slab_cache_set_thread(&cord->slabc);
	if (cord->loop)
		ev_loop_destroy(cord->loop);
	/* Only clean up if initialized. */
	if (cord->fiber_registry) {
		fiber_destroy_all(cord);
		mh_i32ptr_delete(cord->fiber_registry);
	}
	region_destroy(&cord->sched.gc);
	diag_destroy(&cord->sched.diag);
	slab_cache_destroy(&cord->slabc);
}

struct cord_thread_arg
{
	struct cord *cord;
	const char *name;
	void *(*f)(void *);
	void *arg;
	bool is_started;
	pthread_mutex_t start_mutex;
	pthread_cond_t start_cond;
};

/**
 * Cord main thread function. It's not exception-safe, the
 * body function must catch all exceptions instead.
 */
void *cord_thread_func(void *p)
{
	struct cord_thread_arg *ct_arg = (struct cord_thread_arg *) p;
	cord_create(ct_arg->cord, (ct_arg->name));
	/** Can't possibly be the main thread */
	assert(cord()->id != main_thread_id);
	tt_pthread_mutex_lock(&ct_arg->start_mutex);
	void *(*f)(void *) = ct_arg->f;
	void *arg = ct_arg->arg;
	ct_arg->is_started = true;
	tt_pthread_cond_signal(&ct_arg->start_cond);
	tt_pthread_mutex_unlock(&ct_arg->start_mutex);
	void *res = f(arg);
	/*
	 * cord()->on_exit initially holds NULL. This field is
	 * change-once.
	 * Either handler installation succeeds (in cord_cojoin())
	 * or prior to thread exit the thread function discovers
	 * that no handler was installed so far and it stores
	 * CORD_ON_EXIT_WONT_RUN to prevent a future handler
	 * installation (since a handler won't run anyway).
	 */
	const struct cord_on_exit *handler = NULL; /* expected value */
	bool changed;

	changed = pm_atomic_compare_exchange_strong(&cord()->on_exit,
	                                            &handler,
	                                            CORD_ON_EXIT_WONT_RUN);
	if (!changed)
		handler->callback(handler->argument);
	return res;
}

int
cord_start(struct cord *cord, const char *name, void *(*f)(void *), void *arg)
{
	int res = -1;
	struct cord_thread_arg ct_arg = { cord, name, f, arg, false,
		PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };
	tt_pthread_mutex_lock(&ct_arg.start_mutex);
	cord->loop = ev_loop_new(EVFLAG_AUTO | EVFLAG_ALLOCFD);
	if (cord->loop == NULL) {
		diag_set(OutOfMemory, 0, "ev_loop_new", "ev_loop");
		goto end;
	}
	if (tt_pthread_create(&cord->id, NULL,
			      cord_thread_func, &ct_arg) != 0) {
		diag_set(SystemError, "failed to create thread");
		goto end;
	}
	res = 0;
	while (! ct_arg.is_started)
		tt_pthread_cond_wait(&ct_arg.start_cond, &ct_arg.start_mutex);
end:
	if (res != 0) {
		if (cord->loop) {
			ev_loop_destroy(cord->loop);
			cord->loop = NULL;
		}
	}
	tt_pthread_mutex_unlock(&ct_arg.start_mutex);
	tt_pthread_mutex_destroy(&ct_arg.start_mutex);
	tt_pthread_cond_destroy(&ct_arg.start_cond);
	return res;
}

int
cord_join(struct cord *cord)
{
	assert(cord() != cord); /* Can't join self. */
	void *retval = NULL;
	int res = tt_pthread_join(cord->id, &retval);
	if (res == 0) {
		struct fiber *f = cord->fiber;
		if (f->f_ret != 0) {
			assert(!diag_is_empty(&f->diag));
			diag_move(&f->diag, diag_get());
			res = -1;
		}
	} else {
		diag_set(SystemError, "failed to join with thread");
	}
	cord_destroy(cord);
	return res;
}

/** The state of the waiter for a thread to complete. */
struct cord_cojoin_ctx
{
	struct ev_loop *loop;
	/** Waiting fiber. */
	struct fiber *fiber;
	/*
	 * This event is signalled when the subject thread is
	 * about to die.
	 */
	struct ev_async async;
	bool task_complete;
};

static void
cord_cojoin_on_exit(void *arg)
{
	struct cord_cojoin_ctx *ctx = (struct cord_cojoin_ctx *)arg;

	ev_async_send(ctx->loop, &ctx->async);
}

static void
cord_cojoin_wakeup(struct ev_loop *loop, struct ev_async *ev, int revents)
{
	(void)loop;
	(void)revents;

	struct cord_cojoin_ctx *ctx = (struct cord_cojoin_ctx *)ev->data;

	ctx->task_complete = true;
	fiber_wakeup(ctx->fiber);
}

int
cord_cojoin(struct cord *cord)
{
	assert(cord() != cord); /* Can't join self. */

	struct cord_cojoin_ctx ctx;
	ctx.loop = loop();
	ctx.fiber = fiber();
	ctx.task_complete = false;

	ev_async_init(&ctx.async, cord_cojoin_wakeup);
	ctx.async.data = &ctx;
	ev_async_start(loop(), &ctx.async);

	struct cord_on_exit handler = { cord_cojoin_on_exit, &ctx };

	/*
	 * cord->on_exit initially holds a NULL value. This field is
	 * change-once.
	 */
	const struct cord_on_exit *prev_handler = NULL; /* expected value */
	bool changed = pm_atomic_compare_exchange_strong(&cord->on_exit,
	                                                 &prev_handler,
	                                                 &handler);
	/*
	 * A handler installation fails either if the thread did exit or
	 * if someone is already joining this cord (BUG).
	 */
	if (!changed) {
		/* Assume cord's thread already exited. */
		assert(prev_handler == CORD_ON_EXIT_WONT_RUN);
	} else {
		/*
		 * Wait until the thread exits. Prior to exit the
		 * thread invokes cord_cojoin_on_exit, signaling
		 * ev_async, making the event loop call
		 * cord_cojoin_wakeup, waking up this fiber again.
		 *
		 * The fiber is non-cancellable during the wait to
		 * avoid invalidating of the cord_cojoin_ctx
		 * object declared on stack.
		 */
		bool cancellable = fiber_set_cancellable(false);
		fiber_yield();
		/* Spurious wakeup indicates a severe BUG, fail early. */
		if (ctx.task_complete == 0)
			panic("Wrong fiber woken");
		fiber_set_cancellable(cancellable);
	}

	ev_async_stop(loop(), &ctx.async);
	return cord_join(cord);
}

int
break_ev_loop_f(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	ev_break(loop(), EVBREAK_ALL);
	return 0;
}

struct costart_ctx
{
	fiber_func run;
	void *arg;
};

/** Replication acceptor fiber handler. */
static void *
cord_costart_thread_func(void *arg)
{
	struct costart_ctx ctx = *(struct costart_ctx *) arg;
	free(arg);

	struct fiber *f = fiber_new("main", ctx.run);
	if (f == NULL)
		return NULL;

	struct trigger break_ev_loop = {
		RLIST_LINK_INITIALIZER, break_ev_loop_f, NULL, NULL
	};
	/*
	 * Got to be in a trigger, to break the loop even
	 * in case of an exception.
	 */
	trigger_add(&f->on_stop, &break_ev_loop);
	fiber_set_joinable(f, true);
	fiber_start(f, ctx.arg);
	if (!fiber_is_dead(f)) {
		/* The fiber hasn't died right away at start. */
		ev_run(loop(), 0);
	}
	/*
	 * Preserve the exception with which the main fiber
	 * terminated, if any.
	 */
	assert(fiber_is_dead(f));
	fiber()->f_ret = fiber_join(f);

	return NULL;
}

int
cord_costart(struct cord *cord, const char *name, fiber_func f, void *arg)
{
	/** Must be allocated to avoid races. */
	struct costart_ctx *ctx = (struct costart_ctx *) malloc(sizeof(*ctx));
	if (ctx == NULL) {
		diag_set(OutOfMemory, sizeof(struct costart_ctx),
			 "malloc", "costart_ctx");
		return -1;
	}
	ctx->run = f;
	ctx->arg = arg;
	if (cord_start(cord, name, cord_costart_thread_func, ctx) == -1) {
		free(ctx);
		return -1;
	}
	return 0;
}

void
cord_set_name(const char *name)
{
	snprintf(cord()->name, sizeof(cord()->name), "%s", name);
	/* Main thread's name will replace process title in ps, skip it */
	if (cord_is_main())
		return;
	tt_pthread_setname(name);
}

bool
cord_is_main()
{
	return cord() == &main_cord;
}

struct slab_cache *
cord_slab_cache(void)
{
	return &cord()->slabc;
}

static NOINLINE int
check_stack_direction(void *prev_stack_frame)
{
	return __builtin_frame_address(0) < prev_stack_frame ? -1: 1;
}

void
fiber_init(int (*invoke)(fiber_func f, va_list ap))
{
	page_size = sysconf(_SC_PAGESIZE);
	stack_direction = check_stack_direction(__builtin_frame_address(0));
	fiber_invoke = invoke;
	main_thread_id = pthread_self();
	main_cord.loop = ev_default_loop(EVFLAG_AUTO | EVFLAG_ALLOCFD);
	cord_create(&main_cord, "main");
}

void
fiber_free(void)
{
	cord_destroy(&main_cord);
}

int fiber_stat(fiber_stat_cb cb, void *cb_ctx)
{
	struct fiber *fiber;
	struct cord *cord = cord();
	int res;
	rlist_foreach_entry(fiber, &cord->alive, link) {
		res = cb(fiber, cb_ctx);
		if (res != 0)
			return res;
	}
	return 0;
}
