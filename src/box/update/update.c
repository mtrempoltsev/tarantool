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
#include "update.h"
#include "box/error.h"
#include "diag.h"
#include <msgpuck/msgpuck.h>
#include "box/column_mask.h"
#include "fiber.h"
#include "update_field.h"

/**
 * UPDATE is represented by a sequence of operations, each
 * working with a single field. There also are operations which
 * add or remove fields. Only one operation on the same field
 * is allowed.
 * Field is any part of a tuple: top-level array's field, leaf of
 * a complex tuple with lots of maps and arrays inside, a whole
 * map/array inside a tuple.
 *
 * Supported field change operations are: SET, ADD, SUBTRACT;
 * bitwise AND, XOR and OR; SPLICE.
 * Supported tuple change operations are: SET, DELETE, INSERT.
 *
 * If the number of fields in a tuple is altered by an operation,
 * field index of all following operations is evaluated against the
 * new tuple. It applies to internal tuple's arrays too.
 *
 * Despite the allowed complexity, a typical use case for UPDATE
 * is when the operation count is much less than field count in
 * a tuple.
 *
 * With the common case in mind, UPDATE tries to minimize
 * the amount of unnecessary temporary tuple copies.
 *
 * First, operations are parsed and initialized. Then they are
 * applied one by one to a tuple. Each operation may change an
 * already located field in a tuple, or may split parent of the
 * field into subtrees. After all operations are applied, the
 * result is a tree of updated, new, and non-changed fields.
 * The tree needs to be flattened into MessagePack format. For
 * that a resulting tuple length is calculated. Memory for the new
 * tuple is allocated in one contiguous chunk. Then the update
 * tree is stored into the chunk as a result tuple.
 *
 * Note, that the result tree didn't allocate anything until a
 * result was stored. It was referencing old tuple's memory.
 * With this approach, cost of UPDATE is proportional to O(tuple
 * length) + O(C * log C), where C is the number of operations in
 * the request, and data is copied from the old tuple to the new
 * one only once.
 *
 * As long as INSERT and DELETE change the relative field order in
 * arrays and maps, these fields are represented as special
 * structures optimized for updates to provide fast search and
 * don't realloc anything. It is 'rope' data structure for array,
 * and a simpler key-value list sorted by update time for map.
 *
 * A rope is a binary tree designed to store long strings built
 * from pieces. Each tree node points to a substring of a large
 * string. In our case, each rope node points at a range of
 * fields, initially in the old tuple, and then, as fields are
 * added and deleted by UPDATE, in the "current" tuple.
 * Note, that the tuple itself is not materialized: when
 * operations which affect field count are initialized, the rope
 * is updated to reflect the new field order.
 * In particular, if a field is deleted by an operation,
 * it disappears from the rope and all subsequent operations
 * on this field number instead affect the field following the
 * deleted one.
 */

/** Update internal state */
struct tuple_update {
	/** Operations array. */
	struct update_op *ops;
	/** Length of ops. */
	uint32_t op_count;
	/**
	 * Index base for MessagePack update operations. If update
	 * is from Lua, then the base is 1. Otherwise 0. That
	 * field exists because Lua uses 1-based array indexing,
	 * and Lua-to-MessagePack encoder keeps this indexing when
	 * encodes operations array. Index base allows not to
	 * re-encode each Lua update with 0-based indexes.
	 */
	int index_base;
	/** A bitmask of all columns modified by this update. */
	uint64_t column_mask;
	/** First level of update tree. It is always array. */
	struct update_field root;
};

/**
 * Read and check update operations and fill column mask.
 *
 * @param[out] update Update meta.
 * @param expr MessagePack array of operations.
 * @param expr_end End of the @expr.
 * @param dict Dictionary to lookup field number by a name.
 * @param field_count_hint Field count in the updated tuple. If
 *        there is no tuple at hand (for example, when we are
 *        reading UPSERT operations), then 0 for field count will
 *        do as a hint: the only effect of a wrong hint is
 *        a possibly incorrect column_mask.
 *        A correct field count results in an accurate
 *        column mask calculation.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
static int
update_read_ops(struct tuple_update *update, const char *expr,
		const char *expr_end, struct tuple_dictionary *dict,
		int32_t field_count_hint)
{
	if (mp_typeof(*expr) != MP_ARRAY) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "update operations must be an "
			 "array {{op,..}, {op,..}}");
		return -1;
	}
	uint64_t column_mask = 0;
	/* number of operations */
	update->op_count = mp_decode_array(&expr);

	if (update->op_count > BOX_UPDATE_OP_CNT_MAX) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "too many operations for update");
		return -1;
	}

	int size = update->op_count * sizeof(update->ops[0]);
	update->ops = (struct update_op *)
		region_aligned_alloc(&fiber()->gc, size,
				     alignof(struct update_op));
	if (update->ops == NULL) {
		diag_set(OutOfMemory, size, "region_aligned_alloc",
			 "update->ops");
		return -1;
	}
	struct update_op *op = update->ops;
	struct update_op *ops_end = op + update->op_count;
	for (; op < ops_end; op++) {
		if (update_op_decode(op, update->index_base, dict, &expr) != 0)
			return -1;
		/*
		 * Continue collecting the changed columns
		 * only if there are unset bits in the mask.
		 */
		if (column_mask != COLUMN_MASK_FULL) {
			int32_t field_no;
			if (op->field_no >= 0)
				field_no = op->field_no;
			else if (op->opcode != '!')
				field_no = field_count_hint + op->field_no;
			else
				/*
				 * '!' with a negative number
				 * inserts a new value after the
				 * position, specified in the
				 * field_no. Example:
				 * tuple: [1, 2, 3]
				 *
				 * update1: {'#', -1, 1}
				 * update2: {'!', -1, 4}
				 *
				 * result1: [1, 2, * ]
				 * result2: [1, 2, 3, *4]
				 * As you can see, both operations
				 * have field_no -1, but '!' actually
				 * creates a new field. So
				 * set field_no to insert position + 1.
				 */
				field_no = field_count_hint + op->field_no + 1;
			/*
			 * Here field_no can be < 0 only if update
			 * operation encounters a negative field
			 * number N and abs(N) > field_count_hint.
			 * For example, the tuple is: {1, 2, 3},
			 * and the update operation is
			 * {'#', -4, 1}.
			 */
			if (field_no < 0) {
				/*
				 * Turn off column mask for this
				 * incorrect UPDATE.
				 */
				column_mask_set_range(&column_mask, 0);
				continue;
			}

			/*
			 * Update result statement's field count
			 * hint. It is used to translate negative
			 * field numbers into positive ones.
			 */
			if (op->opcode == '!')
				++field_count_hint;
			else if (op->opcode == '#')
				field_count_hint -= (int32_t) op->arg.del.count;

			if (op->opcode == '!' || op->opcode == '#')
				/*
				 * If the operation is insertion
				 * or deletion then it potentially
				 * changes a range of columns by
				 * moving them, so need to set a
				 * range of bits.
				 */
				column_mask_set_range(&column_mask, field_no);
			else
				column_mask_set_fieldno(&column_mask, field_no);
		}
	}

	/* Check the remainder length, the request must be fully read. */
	if (expr != expr_end) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "can't unpack update operations");
		return -1;
	}
	update->column_mask = column_mask;
	return 0;
}

/**
 * Apply update operations to the concrete tuple.
 *
 * @param update Update meta.
 * @param old_data MessagePack array of tuple fields without the
 *        array header.
 * @param old_data_end End of the @old_data.
 * @param part_count Field count in the @old_data.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
static int
update_do_ops(struct tuple_update *update, const char *old_data,
	      const char *old_data_end, uint32_t part_count)
{
	if (update_array_create(&update->root, old_data, old_data_end,
				part_count) != 0)
		return -1;
	struct update_op *op = update->ops;
	struct update_op *ops_end = op + update->op_count;
	for (; op < ops_end; op++) {
		if (op->meta->do_op(op, &update->root) != 0)
			return -1;
	}
	return 0;
}

/*
 * Same as update_do_ops but for upsert.
 * @param suppress_error True, if an upsert error is not critical
 *        and it is enough to simply write the error to the log.
 */
static int
upsert_do_ops(struct tuple_update *update, const char *old_data,
	      const char *old_data_end, uint32_t part_count,
	      bool suppress_error)
{
	if (update_array_create(&update->root, old_data, old_data_end,
				part_count) != 0)
		return -1;
	struct update_op *op = update->ops;
	struct update_op *ops_end = op + update->op_count;
	for (; op < ops_end; op++) {
		if (op->meta->do_op(op, &update->root) == 0)
			continue;
		struct error *e = diag_last_error(diag_get());
		if (e->type != &type_ClientError)
			return -1;
		if (!suppress_error) {
			say_error("UPSERT operation failed:");
			error_log(e);
		}
	}
	return 0;
}

static void
update_init(struct tuple_update *update, int index_base)
{
	memset(update, 0, sizeof(*update));
	update->index_base = index_base;
}

static const char *
update_finish(struct tuple_update *update, uint32_t *p_tuple_len)
{
	uint32_t tuple_len = update_array_sizeof(&update->root);
	char *buffer = (char *) region_alloc(&fiber()->gc, tuple_len);
	if (buffer == NULL) {
		diag_set(OutOfMemory, tuple_len, "region_alloc", "buffer");
		return NULL;
	}
	*p_tuple_len = update_array_store(&update->root, buffer,
					  buffer + tuple_len);
	assert(*p_tuple_len == tuple_len);
	return buffer;
}

int
tuple_update_check_ops(const char *expr, const char *expr_end,
		       struct tuple_dictionary *dict, int index_base)
{
	struct tuple_update update;
	update_init(&update, index_base);
	return update_read_ops(&update, expr, expr_end, dict, 0);
}

const char *
tuple_update_execute(const char *expr,const char *expr_end,
		     const char *old_data, const char *old_data_end,
		     struct tuple_dictionary *dict, uint32_t *p_tuple_len,
		     int index_base, uint64_t *column_mask)
{
	struct tuple_update update;
	update_init(&update, index_base);
	uint32_t field_count = mp_decode_array(&old_data);

	if (update_read_ops(&update, expr, expr_end, dict, field_count) != 0)
		return NULL;
	if (update_do_ops(&update, old_data, old_data_end, field_count))
		return NULL;
	if (column_mask)
		*column_mask = update.column_mask;

	return update_finish(&update, p_tuple_len);
}

const char *
tuple_upsert_execute(const char *expr,const char *expr_end,
		     const char *old_data, const char *old_data_end,
		     struct tuple_dictionary *dict, uint32_t *p_tuple_len,
		     int index_base, bool suppress_error, uint64_t *column_mask)
{
	struct tuple_update update;
	update_init(&update, index_base);
	uint32_t field_count = mp_decode_array(&old_data);

	if (update_read_ops(&update, expr, expr_end, dict, field_count) != 0)
		return NULL;
	if (upsert_do_ops(&update, old_data, old_data_end, field_count,
			  suppress_error))
		return NULL;
	if (column_mask)
		*column_mask = update.column_mask;

	return update_finish(&update, p_tuple_len);
}

const char *
tuple_upsert_squash(const char *expr1, const char *expr1_end,
		    const char *expr2, const char *expr2_end,
		    struct tuple_dictionary *dict, size_t *result_size,
		    int index_base)
{
	const char *expr[2] = {expr1, expr2};
	const char *expr_end[2] = {expr1_end, expr2_end};
	struct tuple_update update[2];
	for (int j = 0; j < 2; j++) {
		update_init(&update[j], index_base);
		if (update_read_ops(&update[j], expr[j], expr_end[j],
				    dict, 0) != 0)
			return NULL;
		mp_decode_array(&expr[j]);
		int32_t prev_field_no = index_base - 1;
		for (uint32_t i = 0; i < update[j].op_count; i++) {
			struct update_op *op = &update[j].ops[i];
			if (op->opcode != '+' && op->opcode != '-' &&
			    op->opcode != '=')
				return NULL;
			if (op->field_no <= prev_field_no)
				return NULL;
			prev_field_no = op->field_no;
		}
	}
	size_t possible_size = expr1_end - expr1 + expr2_end - expr2;
	const uint32_t space_for_arr_tag = 5;
	char *buf = (char *) region_alloc(&fiber()->gc,
					  possible_size + space_for_arr_tag);
	if (buf == NULL) {
		diag_set(OutOfMemory, possible_size + space_for_arr_tag,
			 "region_alloc", "buf");
		return NULL;
	}
	/* reserve some space for mp array header */
	char *res_ops = buf + space_for_arr_tag;
	uint32_t res_count = 0; /* number of resulting operations */

	uint32_t op_count[2] = {update[0].op_count, update[1].op_count};
	uint32_t op_no[2] = {0, 0};
	while (op_no[0] < op_count[0] || op_no[1] < op_count[1]) {
		res_count++;
		struct update_op *op[2] = {update[0].ops + op_no[0],
					   update[1].ops + op_no[1]};
		/*
		 * from:
		 * 0 - take op from first update,
		 * 1 - take op from second update,
		 * 2 - merge both ops
		 */
		uint32_t from;
		uint32_t has[2] = {op_no[0] < op_count[0], op_no[1] < op_count[1]};
		assert(has[0] || has[1]);
		if (has[0] && has[1]) {
			from = op[0]->field_no < op[1]->field_no ? 0 :
			       op[0]->field_no > op[1]->field_no ? 1 : 2;
		} else {
			assert(has[0] != has[1]);
			from = has[1];
		}
		if (from == 2 && op[1]->opcode == '=') {
			/*
			 * If an operation from the second upsert is '='
			 * it is just overwrites any op from the first upsert.
			 * So we just skip op from the first upsert and
			 * copy op from the second
			 */
			mp_next(&expr[0]);
			op_no[0]++;
			from = 1;
		}
		if (from < 2) {
			/* take op from one of upserts */
			const char *copy = expr[from];
			mp_next(&expr[from]);
			size_t copy_size = expr[from] - copy;
			memcpy(res_ops, copy, copy_size);
			res_ops += copy_size;
			op_no[from]++;
			continue;
		}
		/* merge: apply second '+' or '-' */
		assert(op[1]->opcode == '+' || op[1]->opcode == '-');
		if (op[0]->opcode == '-') {
			op[0]->opcode = '+';
			int96_invert(&op[0]->arg.arith.int96);
		}
		struct update_op res;
		if (make_arith_operation(op[1], op[0]->arg.arith,
					 &res.arg.arith) != 0)
			return NULL;
		res_ops = mp_encode_array(res_ops, 3);
		res_ops = mp_encode_str(res_ops,
					(const char *)&op[0]->opcode, 1);
		res_ops = mp_encode_uint(res_ops,
					 op[0]->field_no +
						 update[0].index_base);
		store_op_arith(&res, NULL, res_ops);
		res_ops += update_arith_sizeof(&res.arg.arith);
		mp_next(&expr[0]);
		mp_next(&expr[1]);
		op_no[0]++;
		op_no[1]++;
	}
	assert(op_no[0] == op_count[0] && op_no[1] == op_count[1]);
	assert(expr[0] == expr_end[0] && expr[1] == expr_end[1]);
	char *arr_start = buf + space_for_arr_tag -
		mp_sizeof_array(res_count);
	mp_encode_array(arr_start, res_count);
	*result_size = res_ops - arr_start;
	return arr_start;
}
