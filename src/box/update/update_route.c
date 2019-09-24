/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
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
#include "update_field.h"
#include "fiber.h"
#include "box/tuple.h"

/**
 * Do the actual branch. This is the case when an existing
 * bar/route path is different from a new operation's path in an
 * array.
 * @param[out] next_hop A field which will be initialized as an
 *        array, and which will be a point to apply a new
 *        operation.
 * @param child Current field from which the branch happens. It
 *        already contains an old update subtree.
 * @param parent MessagePack array to be taken by @a next_hop.
 */
static int
update_route_branch_array(struct update_field *next_hop,
			  const struct update_field *child, int32_t field_no,
			  const char *parent)
{
	struct update_op *op = child->bar.op;
	/*
	 * There are rules when a subtree can be just copied as is
	 * from one parent to another.
	 * 1) It should not be a leaf. If it is not leaf, then it
	 *    does not change header and other fields of this
	 *    particular array, and can be safely copied into one
	 *    of its fields. Otherwise check next rules.
	 * 2) It should not be a bar. Because if it is not a bar,
	 *    and is a leaf, then it is scalar. Again, changes
	 *    only its own field, does not affect anything else,
	 *    can be copied. Otherwise go down.
	 * 3) Ok, it is a bar, a leaf. It is a bar with zero path
	 *    length. In this case it should be scalar bar, it is
	 *    checked below. The only non-scalar operations are
	 *    '!' and '#'.
	 *
	 * Why '#' and '!' can't be copied? '!', for example,
	 * being applied to a field [1], affects all fields [2-*],
	 * and the array header. The same but in an even worse
	 * form about '#'. Such operations should be redone. They
	 * affect many fields and the parent.
	 *
	 * There is a tricky thing though - why not to just redo
	 * all operations here, for the sake of code simplicity?
	 * It would allow to remove 'create_with_child' crutch.
	 * The answer is - it is not possible. If a field is
	 * copyable, it is not re-applicable. And vice-versa. For
	 * example, if it is not a leaf, then there may be many
	 * operations, not one. A subtree just can't be
	 * 're-applied'.
	 *
	 * If the operation is scalar and a leaf, then its result
	 * has already overridden its arguments. This is because
	 * scalar operations save result into the arguments, to
	 * save memory. A second operation appliance would lead
	 * to very surprising results.
	 *
	 * Another reason - performance. This path should be
	 * quite hot, and to copy a struct is for sure much faster
	 * than to reapply an operation using a virtual function.
	 * Operations '!' and '#' are quite rare, so their
	 * optimization is not the goal.
	 */
	if (child->type != UPDATE_BAR || child->bar.path_len > 0 ||
	    (op->opcode != '!' && op->opcode != '#')) {
		return update_array_create_with_child(next_hop, child,
						      field_no, parent);
	}
	op->token_type = JSON_TOKEN_NUM;
	op->field_no = field_no;
	const char *data = parent;
	uint32_t field_count = mp_decode_array(&data);
	const char *end = data;
	for (uint32_t i = 0; i < field_count; ++i)
		mp_next(&end);
	if (update_array_create(next_hop, parent, data, end, field_count) != 0)
		return -1;
	return op->meta->do_op(op, next_hop);
}

/**
 * Do the actual branch, but by a map and a key in that map. Works
 * exactly the same as the array-counterpart.
 */
static int
update_route_branch_map(struct update_field *next_hop,
			const struct update_field *child, const char *key,
			int key_len, const char *parent)
{
	struct update_op *op = child->bar.op;
	if (child->type != UPDATE_BAR || child->bar.path_len > 0 ||
	    (op->opcode != '!' && op->opcode != '#')) {
		return update_map_create_with_child(next_hop, child, key,
						    key_len, parent);
	}
	op->token_type = JSON_TOKEN_STR;
	op->key = key;
	op->key_len = key_len;
	const char *data = parent;
	uint32_t field_count = mp_decode_map(&data);
	const char *end = data;
	for (uint32_t i = 0; i < field_count; ++i) {
		mp_next(&end);
		mp_next(&end);
	}
	if (update_map_create(next_hop, parent, data, end, field_count) != 0)
		return -1;
	return op->meta->do_op(op, next_hop);
}

struct update_field *
update_route_branch(struct update_field *field, struct update_op *new_op)
{
	assert(new_op->lexer.src != NULL);
	const char *old_path;
	int old_path_len;
	if (field->type == UPDATE_BAR) {
		old_path = field->bar.path;
		old_path_len = field->bar.path_len;
	} else {
		assert(field->type == UPDATE_ROUTE);
		old_path = field->route.path;
		old_path_len = field->route.path_len;
	}
	assert(old_path != NULL);
	struct json_lexer old_path_lexer;
	struct json_token old_token, new_token;
	/*
	 * Offset is going to be used as a length of the route
	 * node created as a parent of the old subtree and the
	 * new operation. As it is described in the route
	 * definition in struct update_field - route is the common
	 * prefix of all operations of the subtree. Here its
	 * length is calculated.
	 * In addition, it is used here to determine if the new
	 * operation is different from the current subtree from
	 * the very beginning. Basically, it means the offset is
	 * 0, and no route is generated. Root becomes a regular
	 * update field (array, map), not a route. Example:
	 * @a field is a bar '[1].a.b = 20', @a new_op is
	 * '[2].c.d = 30'. In this case the paths are already
	 * different from the beginning, no common prefix = no
	 * route. Array with children [1].a.b and [2].c.d becomes
	 * a root.
	 */
	int saved_old_offset;
	json_lexer_create(&old_path_lexer, old_path, old_path_len,
			  TUPLE_INDEX_BASE);
	const char *parent = field->data;
	int rc;
	do {
		saved_old_offset = old_path_lexer.offset;
		rc = json_lexer_next_token(&old_path_lexer, &old_token);
		/* Old path is already validated. */
		assert(rc == 0);
		rc = json_lexer_next_token(&new_op->lexer, &new_token);
		if (rc != 0) {
			update_err_bad_json(new_op, rc);
			return NULL;
		}
		if (json_token_cmp(&old_token, &new_token) != 0)
			break;
		switch(new_token.type) {
		case JSON_TOKEN_NUM:
			rc = tuple_field_go_to_index(&parent, new_token.num);
			break;
		case JSON_TOKEN_STR:
			rc = tuple_field_go_to_key(&parent, new_token.str,
						   new_token.len);
			break;
		default:
			/*
			 * Can't be ANY, because old and new
			 * tokens are equal, but '*' is invalid in
			 * paths and old was already checked for
			 * that.
			 */
			assert(new_token.type == JSON_TOKEN_END);
			update_err_double(new_op);
			return NULL;
		}
		/*
		 * Must always find a field, because old
		 * token already went that path.
		 */
		assert(rc == 0);
	} while (true);
	enum mp_type type = mp_typeof(*parent);
	/*
	 * The paths are different from the very beginning. It
	 * means, that the old field should be transformed instead
	 * of creating a new route node.
	 */
	bool transform_root = saved_old_offset == 0;
	struct update_field *next_hop;
	if (! transform_root) {
		next_hop = (struct update_field *)
			region_alloc(&fiber()->gc, sizeof(*next_hop));
		if (next_hop == NULL) {
			diag_set(OutOfMemory, sizeof(*next_hop), "region_alloc",
				 "next_hop");
			return NULL;
		}
	} else {
		next_hop = field;
	}

	int path_offset = old_path_lexer.offset;
	struct update_field child = *field;
	if (child.type == UPDATE_ROUTE) {
		child.route.path += path_offset;
		child.route.path_len -= path_offset;
		if (child.route.path_len == 0)
			child = *child.route.next_hop;
	} else {
		assert(child.type == UPDATE_BAR);
		child.bar.path += path_offset;
		child.bar.path_len -= path_offset;
		/*
		 * Yeah, bar length can become 0 here, but it is
		 * ok as long as it is not a non-scalar operation
		 * like '!' or '#'. In these cases it changes the
		 * parent too. When a bar is scalar ('=', arith,
		 * splice, ...), it operates on one concrete field
		 * and works even if its path len is 0. Of course,
		 * it is not a big deal to fix such bar here
		 * making it scalar, but it would be additional
		 * branching on an actually hot path.
		 * Talking of '#' and '!' - they are handled by
		 * array and map 'branchers' internally, below.
		 */
	}

	if (type == MP_ARRAY) {
		if (new_token.type != JSON_TOKEN_NUM) {
			update_err(new_op, "can not update array by "\
				   "non-integer index");
			return NULL;
		}
		new_op->token_type = JSON_TOKEN_NUM;
		new_op->field_no = new_token.num;
		if (update_route_branch_array(next_hop, &child, old_token.num,
					      parent) != 0)
			return NULL;
	} else if (type == MP_MAP) {
		if (new_token.type != JSON_TOKEN_STR) {
			update_err(new_op, "can not update map by "\
				   "non-string key");
			return NULL;
		}
		new_op->token_type = JSON_TOKEN_STR;
		new_op->key = new_token.str;
		new_op->key_len = new_token.len;
		if (update_route_branch_map(next_hop, &child, old_token.str,
					    old_token.len, parent) != 0)
			return NULL;
	} else {
		update_err_no_such_field(new_op);
		return NULL;
	}

	if (! transform_root) {
		field->type = UPDATE_ROUTE;
		field->route.path = old_path;
		field->route.path_len = saved_old_offset;
		field->route.next_hop = next_hop;
	}
	return next_hop;
}

/**
 * Obtain a next node of the update tree to which @a op should be
 * propagated. It is the same as branch, but has a fast path in
 * case of @a field is route, and operation prefix matches this
 * route - then no need to parse JSON and dive into MessagePack,
 * the route is just followed, via a lexer offset increase.
 *
 * @param field Field to find where to apply @a op.
 * @param op New operation to apply.
 * @return A field to which @a op should be applied.
 */
static struct update_field *
update_route_next(struct update_field *field, struct update_op *op)
{
	assert(field->type == UPDATE_ROUTE);
	assert(! update_op_is_term(op));
	const char *new_path = op->lexer.src + op->lexer.offset;
	int new_path_len = op->lexer.src_len - op->lexer.offset;
	if (field->route.path_len <= new_path_len &&
	    memcmp(field->route.path, new_path, field->route.path_len) == 0) {
		/*
		 * Fast path: jump to the next hop with no
		 * decoding. Is used, when several JSON updates
		 * have the same prefix.
		 */
		op->lexer.offset += field->route.path_len;
		return field->route.next_hop;
	}
	return update_route_branch(field, op);
}

#define DO_SCALAR_OP_GENERIC(op_type)						\
int										\
do_op_route_##op_type(struct update_op *op, struct update_field *field)		\
{										\
	assert(field->type == UPDATE_ROUTE);					\
	struct update_field *next_hop = update_route_next(field, op);		\
	if (next_hop == NULL)							\
		return -1;							\
	return do_op_##op_type(op, next_hop);					\
}

DO_SCALAR_OP_GENERIC(set)

DO_SCALAR_OP_GENERIC(insert)

DO_SCALAR_OP_GENERIC(delete)

DO_SCALAR_OP_GENERIC(arith)

DO_SCALAR_OP_GENERIC(bit)

DO_SCALAR_OP_GENERIC(splice)

uint32_t
update_route_sizeof(struct update_field *field)
{
	return field->size - field->route.next_hop->size +
	       update_field_sizeof(field->route.next_hop);
}

uint32_t
update_route_store(struct update_field *field, char *out, char *out_end)
{
	char *saved_out = out;
	int before_hop = field->route.next_hop->data - field->data;
	memcpy(out, field->data, before_hop);
	out += before_hop;
	out += update_field_store(field->route.next_hop, out, out_end);
	int after_hop = before_hop + field->route.next_hop->size;
	memcpy(out, field->data + after_hop, field->size - after_hop);
	return out + field->size - after_hop - saved_out;
}
