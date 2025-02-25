#ifndef TARANTOOL_LUA_UTILS_H_INCLUDED
#define TARANTOOL_LUA_UTILS_H_INCLUDED
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
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

#include <stdint.h>
#include <string.h>
#include <math.h> /* modf, isfinite */

#include <msgpuck.h> /* enum mp_type */
#include "trigger.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include <lua.h>
#include <lauxlib.h> /* luaL_error */

#include <lj_state.h>
#include <lj_obj.h>
#include <lj_ctype.h>
#include <lj_cdata.h>
#include <lj_cconv.h>
#include <lj_lib.h>
#include <lj_tab.h>
#include <lj_meta.h>

#include "lua/error.h"

#include "lib/core/mp_extension_types.h"
#include "lib/core/decimal.h" /* decimal_t */

struct lua_State;
struct ibuf;

/**
 * Single global lua_State shared by core and modules.
 * Created with tarantool_lua_init().
 * const char *msg = lua_tostring(L, -1);
 * snprintf(m_errmsg, sizeof(m_errmsg), "%s", msg ? msg : "");
 */
extern struct lua_State *tarantool_L;
extern struct ibuf *tarantool_lua_ibuf;

extern uint32_t CTID_CONST_CHAR_PTR;
extern uint32_t CTID_CHAR_PTR;

/** \cond public */

/**
 * @brief Push cdata of given \a ctypeid onto the stack.
 * CTypeID must be used from FFI at least once. Allocated memory returned
 * uninitialized. Only numbers and pointers are supported.
 * @param L Lua State
 * @param ctypeid FFI's CTypeID of this cdata
 * @sa luaL_checkcdata
 * @return memory associated with this cdata
 */
LUA_API void *
luaL_pushcdata(struct lua_State *L, uint32_t ctypeid);

/**
 * @brief Checks whether the function argument idx is a cdata
 * @param L Lua State
 * @param idx stack index
 * @param ctypeid FFI's CTypeID of this cdata
 * @sa luaL_pushcdata
 * @return memory associated with this cdata
 */
LUA_API void *
luaL_checkcdata(struct lua_State *L, int idx, uint32_t *ctypeid);

/**
 * @brief Sets finalizer function on a cdata object.
 * Equivalent to call ffi.gc(obj, function).
 * Finalizer function must be on the top of the stack.
 * @param L Lua State
 * @param idx object
 */
LUA_API void
luaL_setcdatagc(struct lua_State *L, int idx);

/**
* @brief Return CTypeID (FFI) of given СDATA type
* @param L Lua State
* @param ctypename С type name as string (e.g. "struct request" or "uint32_t")
* @sa luaL_pushcdata
* @sa luaL_checkcdata
* @return CTypeID
*/
LUA_API uint32_t
luaL_ctypeid(struct lua_State *L, const char *ctypename);

/**
* @brief Declare symbols for FFI
* @param L Lua State
* @param ctypename C definitions, e.g "struct stat"
* @sa ffi.cdef(def)
* @retval 0 on success
* @retval LUA_ERRRUN, LUA_ERRMEM, LUA_ERRERR otherwise
*/
LUA_API int
luaL_cdef(struct lua_State *L, const char *ctypename);

/** \endcond public */

/**
 * @brief Return CTypeID (FFI) of given CDATA type,
 * register a metatable with \a methods to be
 * associated with every value of the given
 * type on its creation iva FFI.
 * @sa luaL_register_type
 * @sa luaL_ctypeid
 * @return CTypeID
 */
uint32_t
luaL_metatype(struct lua_State *L, const char *ctypename,
	      const struct luaL_Reg *methods);

static inline lua_Integer
luaL_arrlen(struct lua_State *L, int idx)
{
	lua_Integer max = 0;
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		lua_pop(L, 1); /* pop the value */
		if (lua_type(L, -1) != LUA_TNUMBER)
			continue;
		lua_Number k = lua_tonumber(L, -1);
		if (k <= max || floor(k) != k)
			continue;
		max = k;
	}
	return max;
}

static inline lua_Integer
luaL_maplen(struct lua_State *L, int idx)
{
	lua_Integer size = 0;
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		lua_pop(L, 1); /* pop the value */
		size++;
	}
	return size;
}

/**
 * Common configuration options for Lua serializers (MsgPack, YAML, JSON)
 */
struct luaL_serializer {
	/**
	 * luaL_tofield tries to classify table into one of four kinds
	 * during encoding:
	 *
	 *  + map - at least one table index is not unsigned integer.
	 *  + regular array - all array indexes are available.
	 *  + sparse array - at least one array index is missing.
	 *  + excessively sparse arrat - the number of values missing
	 * exceeds the configured ratio.
	 *
	 * An array is excessively sparse when **all** the following
	 * conditions are met:
	 *
	 *  + encode_sparse_ratio > 0.
	 *  + max(table) > encode_sparse_safe.
	 *  + max(table) > count(table) * encode_sparse_ratio.
	 *
	 * luaL_tofield will never consider an array to be excessively sparse
	 * when encode_sparse_ratio = 0. The encode_sparse_safe limit ensures
	 * that small Lua arrays are always encoded as sparse arrays.
	 * By default, attempting to encode an excessively sparse array will
	 * generate an error. If encode_sparse_convert is set to true,
	 * excessively sparse arrays will be handled as maps.
	 *
	 * This conversion logic is modeled after Mark Pulford's CJSON module.
	 * @sa http://www.kyne.com.au/~mark/software/lua-cjson-manual.html
	 */
	int encode_sparse_convert;
	/** @see encode_sparse_convert */
	int encode_sparse_ratio;
	/** @see encode_sparse_convert */
	int encode_sparse_safe;
	/** Max recursion depth for encoding (MsgPack, CJSON only) */
	int encode_max_depth;
	/**
	 * A flag whether a table with too high nest level should
	 * be cropped. The not-encoded fields are replaced with
	 * one null. If not set, too high nesting is considered an
	 * error.
	 */
	int encode_deep_as_nil;
	/** Enables encoding of NaN and Inf numbers */
	int encode_invalid_numbers;
	/** Floating point numbers precision (YAML, CJSON only) */
	int encode_number_precision;

	/**
	 * Enables __serialize meta-value checking:
	 *
	 *  + 'seq', 'sequence', 'array' - table encoded as an array
	 *  + 'map', 'mappping' - table encoded as a map.
	 *    'seq' or 'map' also enable flow (compact) mode for YAML serializer
	 *    (flow="[1,2,3]" vs block=" - 1\n - 2\n - 3\n").
	 *  + function - the meta-method is called to unpack serializable
	 *    representation of table, cdata or userdata objects.
	 */
	int encode_load_metatables;
	/** Enables tostring() usage for unknown types */
	int encode_use_tostring;
	/** Use NULL for all unrecognizable types */
	int encode_invalid_as_nil;

	/** Enables decoding NaN and Inf numbers */
	int decode_invalid_numbers;
	/** Save __serialize meta-value for decoded arrays and maps */
	int decode_save_metatables;
	/** Max recursion depts for decoding (CJSON only) */
	int decode_max_depth;

	/** Enable support for compact represenation (internal, YAML-only). */
	int has_compact;
	/**
	 * Border where copyable fields end. Is used to copy
	 * serializer options into an existing serializer without
	 * erasure of its non-option fields like triggers.
	 */
	char end_of_options[0];
	/**
	 * Trigger object to subscribe on updates of a more
	 * general serializer. For example, tuple serializer
	 * subscribes on msgpack.
	 */
	struct trigger update_trigger;
	/**
	 * List of triggers on update of this serializer. To push
	 * updates down to dependent serializers.
	 */
	struct rlist on_update;
};

extern int luaL_nil_ref;
extern int luaL_map_metatable_ref;
extern int luaL_array_metatable_ref;

#define LUAL_SERIALIZER "serializer"
#define LUAL_SERIALIZE "__serialize"

struct luaL_serializer *
luaL_newserializer(struct lua_State *L, const char *modname, const luaL_Reg *reg);

/**
 * Copy all option fields of @a src into @a dst. Other fields,
 * such as triggers, are not touched.
 */
void
luaL_serializer_copy_options(struct luaL_serializer *dst,
			     const struct luaL_serializer *src);

static inline struct luaL_serializer *
luaL_checkserializer(struct lua_State *L) {
	return (struct luaL_serializer *)
		luaL_checkudata(L, lua_upvalueindex(1), LUAL_SERIALIZER);
}

/**
 * Initialize serializer with default parameters.
 * @param cfg Serializer to inherit configuration.
 */
void
luaL_serializer_create(struct luaL_serializer *cfg);

/**
 * Parse configuration table into @a cfg.
 * @param L Lua stack.
 * @param cfg Serializer to inherit configuration.
 */
void
luaL_serializer_parse_options(struct lua_State *l,
			      struct luaL_serializer *cfg);

/** A single value on the Lua stack. */
struct luaL_field {
	union {
		struct {
			const char *data;
			uint32_t len;
		} sval;
		int64_t ival;
		double dval;
		float fval;
		bool bval;
		/* Array or map. */
		uint32_t size;
		decimal_t *decval;
	};
	enum mp_type type;
	/* subtypes of MP_EXT */
	enum mp_extension_type ext_type;
	bool compact;                /* a flag used by YAML serializer */
};

/**
 * @brief Convert a value from the Lua stack to a lua_field structure.
 * This function is designed for use with Lua bindings and data
 * serialization functions (YAML, MsgPack, JSON, etc.).
 *
 * Conversion rules:
 * - LUA_TNUMBER when is integer and >= 0 -> UINT
 * - LUA_TNUMBER when is integer and < 0 -> INT
 * - LUA_TNUMBER when is not integer -> DOUBLE
 * - LUA_TBOOLEAN -> BOOL
 * - LUA_TSTRING -> STRING
 * - LUA_TNIL -> NIL
 * - LUA_TTABLE when is array -> ARRAY
 * - LUA_TTABLE when is not array -> MAP
 * - LUA_TUSERDATA, LUA_TLIGHTUSERDATA, CTID_P_VOID when == NULL -> NIL
 * - CTID_INT*, CTID_CCHAR when >= 0 -> UINT
 * - CTID_INT*, CTID_CCHAR when < 0 -> INT
 * - CTID_FLOAT -> FLOAT
 * - CTID_DOUBLE -> DOUBLE
 * - CTID_BOOL -> BOOL
 * - otherwise -> EXT
 *
 * ARRAY vs MAP recognition works based on encode_sparse_convert,
 * encode_sparse_ratio, encode_sparse_safe and encode_load_metatables config
 * parameters (see above). Tables are not saved to lua_field structure and
 * should be processed manually, according to returned type and size value.
 *
 * This function doesn't try to unpack unknown types and simple returns MP_EXT.
 * The caller can use luaL_tofield() for basic conversion, then invoke internal
 * hooks(if available) and then call luaL_checkfield(), which will try to
 * unpack cdata/userdata objects or raise and error.
 *
 * @param L stack
 * @param cfg configuration
 * @param index stack index
 * @param field conversion result
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
int
luaL_tofield(struct lua_State *L, struct luaL_serializer *cfg, int index,
	     struct luaL_field *field);

/**
 * @brief Try to convert userdata/cdata values using defined conversion logic.
 * Must be used only after lua_tofield().
 *
 * @param L stack
 * @param cfg configuration
 * @param idx stack index
 * @param field conversion result
 */
void
luaL_convertfield(struct lua_State *L, struct luaL_serializer *cfg, int idx,
		  struct luaL_field *field);

/**
 * @brief A wrapper for luaL_tofield() and luaL_convertfield() that
 * tries to convert value or raise an error.
 * @param L stack
 * @param cfg configuration
 * @param idx stack index
 * @param field conversion result
 * @sa lua_tofield()
 * @sa luaL_convertfield()
 *
 * Common conversion order for tables:
 * size/count detection -> (sparse array checking) -> (__serialize)
 *
 * Common conversion order for userdata/cdata objects:
 * (internal trigger) -> (__serialize) -> (tostring) -> (nil) -> exception
 *
 * Common conversion order for other types:
 * (tostring) -> (nil) -> exception
 */
static inline void
luaL_checkfield(struct lua_State *L, struct luaL_serializer *cfg, int idx,
		struct luaL_field *field)
{
	if (luaL_tofield(L, cfg, idx, field) < 0)
		luaT_error(L);
	if (field->type != MP_EXT || field->ext_type != MP_UNKNOWN_EXTENSION)
		return;
	luaL_convertfield(L, cfg, idx, field);
}

void
luaL_register_type(struct lua_State *L, const char *type_name,
		   const struct luaL_Reg *methods);


void
luaL_register_module(struct lua_State *L, const char *modname,
		     const struct luaL_Reg *methods);

/** \cond public */

/**
 * Push uint64_t onto the stack
 *
 * @param L is a Lua State
 * @param val is a value to push
 */
LUA_API void
luaL_pushuint64(struct lua_State *L, uint64_t val);

/**
 * Push int64_t onto the stack
 *
 * @param L is a Lua State
 * @param val is a value to push
 */
LUA_API void
luaL_pushint64(struct lua_State *L, int64_t val);

/**
 * Checks whether the argument idx is a uint64 or a convertable string and
 * returns this number.
 * \throws error if the argument can't be converted.
 */
LUA_API uint64_t
luaL_checkuint64(struct lua_State *L, int idx);

/**
 * Checks whether the argument idx is a int64 or a convertable string and
 * returns this number.
 * \throws error if the argument can't be converted.
 */
LUA_API int64_t
luaL_checkint64(struct lua_State *L, int idx);

/**
 * Checks whether the argument idx is a uint64 or a convertable string and
 * returns this number.
 * \return the converted number or 0 of argument can't be converted.
 */
LUA_API uint64_t
luaL_touint64(struct lua_State *L, int idx);

/**
 * Checks whether the argument idx is a int64 or a convertable string and
 * returns this number.
 * \return the converted number or 0 of argument can't be converted.
 */
LUA_API int64_t
luaL_toint64(struct lua_State *L, int idx);

/**
 * Like lua_call(), but with the proper support of Tarantool errors.
 * \sa lua_call()
 */
LUA_API int
luaT_call(lua_State *L, int nargs, int nreturns);

/**
 * Like lua_cpcall(), but with the proper support of Tarantool errors.
 * \sa lua_cpcall()
 */
LUA_API int
luaT_cpcall(lua_State *L, lua_CFunction func, void *ud);

/**
 * Get global Lua state used by Tarantool
 */
LUA_API lua_State *
luaT_state(void);

/**
 * Like lua_tolstring, but supports metatables, booleans and nil properly.
 */
LUA_API const char *
luaT_tolstring(lua_State *L, int idx, size_t *ssize);

/**
 * Check whether a Lua object is a function or has
 * metatable/metatype with a __call field.
 *
 * Note: It does not check type of __call metatable/metatype
 * field.
 */
LUA_API int
luaL_iscallable(lua_State *L, int idx);

/** \endcond public */

/**
 * Convert the last value on the stack into Tarantool error and
 * set diagnostics.
 */
int
luaT_toerror(lua_State *L);

/**
 * Push Lua Table with __serialize = 'map' hint onto the stack.
 * Tables with __serialize hint are properly handled by all serializers.
 * @param L stack
 * @param idx index in the stack
 */
static inline void
luaL_setmaphint(struct lua_State *L, int idx)
{
	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	assert(lua_type(L, idx) == LUA_TTABLE);
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaL_map_metatable_ref);
	lua_setmetatable(L, idx);
}

/**
 * Push Lua Table with __serialize = 'seq' hint onto the stack.
 * Tables with __serialize hint are properly handled by all serializers.
 * @param L stack
 * @param idx index in the stack
 */
static inline void
luaL_setarrayhint(struct lua_State *L, int idx)
{
	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	assert(lua_type(L, idx) == LUA_TTABLE);
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaL_array_metatable_ref);
	lua_setmetatable(L, idx);
}

/**
 * Push ffi's NULL (cdata<void *>: NULL) onto the stack.
 * Can be used as replacement of nil in Lua tables.
 * @param L stack
 */
static inline void
luaL_pushnull(struct lua_State *L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaL_nil_ref);
}

/**
 * Return true if the value at Lua stack is ffi's NULL
 * (cdata<void *>: NULL).
 * @param L stack
 * @param idx stack index
 */
static inline bool
luaL_isnull(struct lua_State *L, int idx)
{
	if (lua_type(L, idx) == LUA_TCDATA) {
		GCcdata *cd = cdataV(L->base + idx - 1);
		return cd->ctypeid == CTID_P_VOID &&
			*(void **)cdataptr(cd) == NULL;
	}
	return false;
}

static inline void
luaL_checkfinite(struct lua_State *L, struct luaL_serializer *cfg,
		 lua_Number number)
{
	if (!cfg->decode_invalid_numbers && !isfinite(number))
		luaL_error(L, "number must not be NaN or Inf");
}

/**
 * @brief A wrapper for lua_newthread() to pass it into luaT_cpcall
 * @param L is a Lua State
 * @sa lua_newthread()
 */
static inline int
luaT_newthread_wrapper(lua_State *L)
{
	*(lua_State **)lua_touserdata(L, 1) = lua_newthread(L);
	return 0;
}

/**
 * @brief Safe wrapper for lua_newthread()
 * @param L is a Lua State
 * @sa lua_newthread()
 */
static inline lua_State *
luaT_newthread(lua_State *L)
{
	lua_State *L1 = NULL;
	if (luaT_cpcall(L, luaT_newthread_wrapper, &L1) != 0) {
		return NULL;
	}
	assert(L1 != NULL);
	setthreadV(L, L->top, L1);
	incr_top(L);
	return L1;
}

/**
 * Check if a value on @a L stack by index @a idx is an ibuf
 * object. Both 'struct ibuf' and 'struct ibuf *' are accepted.
 * Returns NULL, if can't convert - not an ibuf object.
 */
struct ibuf *
luaL_checkibuf(struct lua_State *L, int idx);

/**
 * Check if a value on @a L stack by index @a idx is pointer at
 * char or const char. '(char *)NULL' is also considered a valid
 * char pointer.
 */
int
luaL_checkconstchar(struct lua_State *L, int idx, const char **res);

/* {{{ Helper functions to interact with a Lua iterator from C */

/**
 * Holds iterator state (references to Lua objects).
 */
struct luaL_iterator;

/**
 * Create a Lua iterator from a gen, param, state triplet.
 *
 * If idx == 0, then three top stack values are used as the
 * triplet. Note: they are not popped.
 *
 * Otherwise idx is index on Lua stack points to a
 * {gen, param, state} table.
 */
struct luaL_iterator *
luaL_iterator_new(lua_State *L, int idx);

/**
 * Move iterator to the next value. Push values returned by
 * gen(param, state).
 *
 * Return count of pushed values. Zero means no more results
 * available. In case of a Lua error in a gen function return -1
 * and set a diag.
 */
int
luaL_iterator_next(lua_State *L, struct luaL_iterator *it);

/**
 * Free all resources held by the iterator.
 */
void luaL_iterator_delete(struct luaL_iterator *it);

/* }}} */

int
tarantool_lua_utils_init(struct lua_State *L);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_LUA_UTILS_H_INCLUDED */
