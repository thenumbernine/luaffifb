/* vim: ts=4 sw=4 sts=4 et tw=78
 * Portions copyright (c) 2015-present, Facebook, Inc. All rights reserved.
 * Portions copyright (c) 2011 James R. McKaskill.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include "luaffifb/ffi.h"
#include "luaffifb/ctype.h"
#include "luaffifb/parser.h"
#include "luaffifb/ffi_complex.h"
#include "luaffifb/call.h"
#include "dynasm/dasm_proto.h"
#include <math.h>
#include <inttypes.h>

// Set to 1 to get extra debugging on print
#define DEBUG_TOSTRING 0

int jit_key;
int ctype_mt_key;
int cdata_mt_key;
int callback_mt_key;
int cmodule_mt_key;
int constants_key;
int types_key;
int gc_key;
int callbacks_key;
int functions_key;
int abi_key;
int next_unnamed_key;
int niluv_key;
int asmname_key;

/*
Sets stack[tableLoc][key] = boolean(value)
Leaves the stack.
*/
void setFieldBool(
	lua_State * L,
	int tableLoc,
	char const * key,
	int value
) {											// stack: ...
	tableLoc = lua_absindex(L, tableLoc);
	lua_pushboolean(L, value);				// stack: ..., boolean value
	lua_setfield(L, tableLoc, key);			// stack: ...;  stack[tableLoc][key] = value
}

/*
Sets stack[tableLoc][key] = true.
Leaves the stack.
*/
void setFieldTrue(lua_State * L, int tableLoc, char const * key) {
	setFieldBool(L, tableLoc, key, 1);
}

/*
Sets stack[tableLoc][key] = string(value)
Leaves the stack.
*/
#define setFieldStrLit(\
	/*lua_State * */L,\
	/*int */tableLoc,\
	/*char const * */key,\
	/*char const * */value\
) {											/* stack: ... */\
	int _tableLoc = lua_absindex(L, tableLoc);\
	lua_pushliteral(L, value);				/* stack: ..., string value */\
	lua_setfield(L, _tableLoc, key);			/* stack: ...;  stack[tableLoc][key] = value */\
}

// TODO just implement lua_rawgetp / lua_rawsetp

// Pushes registry[key]
void pushRegistry(lua_State* L, void * key) {
	lua_pushlightuserdata(L, key);		// stack: key
	lua_rawget(L, LUA_REGISTRYINDEX);	// stack: registry[key]
}

// Pops the top value from the stack and assigns it to registry[key]
void setRegistry(
	lua_State * L,
	void * key
) {										// stack: ..., value
	lua_pushlightuserdata(L, key);		// stack: ..., value, key
	lua_insert(L, -2);					// stack: ..., key, value
	lua_rawset(L, LUA_REGISTRYINDEX);	// stack: ...;  registry[key] = value
}


// Returns whether the stack at idx is equal to registry[key]
// Leaves the stack the same.
int equalsRegistry(
	lua_State * L,
	int idx,
	void * key
) {										// stack: ...
	lua_pushvalue(L, idx);				// stack: ..., stack[idx]
	pushRegistry(L, key);				// stack: ..., stack[idx], registry[key]
	int ret = lua_rawequal(L, -2, -1);	// stack: ..., stack[idx], registry[key]
	lua_pop(L, 2);						// stack: ...
	return ret;
}

static void type_error(
	lua_State * L,
	int idx,
	const char * typenameTo,
	int ctypeUserValueLoc,
	const CType * ctypeTo
) {
	// Weird, this function has 3 arguments to handle two different calling pathways internally within the library.
	// Someone should've wrote two separate functions...
	assert(typenameTo || (ctypeUserValueLoc && ctypeTo));

	if (ctypeUserValueLoc) {
		ctypeUserValueLoc = lua_absindex(L, ctypeUserValueLoc);
	}

	idx = lua_absindex(L, idx);

	luaL_Buffer B;
	luaL_buffinit(L, &B);

	CType ctypeFrom;
	to_cdata(L, idx, &ctypeFrom);	// stack: ..., uv = stack[idx]'s uservalue[1] (if it is CData, nil otherwise)

	if (ctypeFrom.type != INVALID_TYPE) {
		push_type_name(L, -1, &ctypeFrom);	// stack: ..., uv, typenameFrom
		// TODO for my newly cdata-enclosed-functions this is giving errors.
		lua_pushfstring(L, "cannot convert argument %d from cdata<%s> to cdata<", idx, lua_tostring(L, -1));	// stack: ..., uv, typenameFrom, str
		lua_remove(L, -2);					// stack: ..., uv, str
		luaL_addvalue(&B);					// stack: ..., uv
	} else {
		lua_pushfstring(L, "cannot convert argument %d from lua<%s> to cdata<", idx, luaL_typename(L, idx));	// stack: ..., uv, str
		luaL_addvalue(&B);					// stack: ..., uv
	}

	if (ctypeTo) {
		push_type_name(L, ctypeUserValueLoc, ctypeTo);	// stack: ..., uv, str2
		luaL_addvalue(&B);					// stack: ...
	} else {
		luaL_addstring(&B, typenameTo);		// stack: ...
	}

	luaL_addchar(&B, '>');					// stack: ...

	luaL_pushresult(&B);					// stack: ..., msg
	lua_error(L);							// stack: ...
}

static void* userdata_toptr(lua_State* L, int idx)
{
	void* ptr = lua_touserdata(L, idx);

	// check for FILE*
	lua_getmetatable(L, idx);
	luaL_getmetatable(L, LUA_FILEHANDLE);
	int isfile = lua_rawequal(L, -1, -2);
	lua_pop(L, 2);

	if (isfile) {
#if LUA_VERSION_NUM == 501
		FILE** stream = (FILE**) ptr;
		return *stream;
#else
		luaL_Stream* stream = (luaL_Stream*) ptr;
		return stream->f;
#endif
	}

	return ptr;
}

static int cdata_tointeger(lua_State* L, int idx, ptrdiff_t* val)
{
	CType ct;
	void* addr = to_cdata(L, idx, &ct);
	lua_pop(L, 1);

	if (ct.pointers) {
		return 0;
	}

	switch (ct.type) {
	case INT8_TYPE:
		*val = *(int8_t*)addr;
		return 1;
	case INT16_TYPE:
		*val = *(int16_t*)addr;
		return 1;
	case INT32_TYPE:
		*val = *(int32_t*)addr;
		return 1;
	case INT64_TYPE:
		*val = *(int64_t*)addr;
		return 1;
	default:
		return 0;
	}
}

static int64_t check_intptr(lua_State* L, int idx, void* p, CType* ct)
{
	if (ct->type == INVALID_TYPE) {
		int64_t ret;
		memset(ct, 0, sizeof(*ct));
		ct->base_size = 8;
		ct->type = INT64_TYPE;
		ct->is_defined = 1;
		ret = luaL_checknumber(L, idx);
		return ret;

	} else if (ct->pointers) {
		return (intptr_t) p;
	}

	switch (ct->type) {
	case INTPTR_TYPE:
	case FUNCTION_PTR_TYPE:
	case FUNCTION_TYPE:
		return *(intptr_t*) p;

	case INT64_TYPE:
		return *(int64_t*) p;

	case INT32_TYPE:
		return ct->is_unsigned ? (int64_t) *(uint32_t*) p : (int64_t) *(int32_t*) p;

	case INT16_TYPE:
		return ct->is_unsigned ? (int64_t) *(uint16_t*) p : (int64_t) *(int16_t*) p;

	case INT8_TYPE:
		return ct->is_unsigned ? (int64_t) *(uint8_t*) p : (int64_t) *(int8_t*) p;

	default:
		type_error(L, idx, "intptr_t", 0, NULL);
		return 0;
	}
}

static int get_cfunction_address(lua_State* L, int idx, CFunction* addr);

#define TO_NUMBER(TYPE, ALLOW_POINTERS, LUA_TONUMBER)	                   \
	TYPE ret = 0;                                                           \
	void* p;                                                                \
	CType ct;                                                        \
	CFunction f;                                                            \
	                                                                        \
	switch (lua_type(L, idx)) {                                             \
	case LUA_TBOOLEAN:                                                      \
	    ret = (TYPE) lua_toboolean(L, idx);                                 \
	    break;                                                              \
	                                                                        \
	case LUA_TNUMBER:                                                       \
	    ret = (TYPE) LUA_TONUMBER(L, idx);                                  \
	    break;                                                              \
	                                                                        \
	case LUA_TSTRING:                                                       \
	    if (!ALLOW_POINTERS) {                                              \
	        type_error(L, idx, #TYPE, 0, NULL);                             \
	    }                                                                   \
	    ret = (TYPE) (intptr_t) lua_tostring(L, idx);                       \
	    break;                                                              \
	                                                                        \
	case LUA_TLIGHTUSERDATA:                                                \
	    if (!ALLOW_POINTERS) {                                              \
	        type_error(L, idx, #TYPE, 0, NULL);                             \
	    }                                                                   \
	    ret = (TYPE) (intptr_t) lua_topointer(L, idx);                      \
	    break;                                                              \
	                                                                        \
	case LUA_TFUNCTION:                                                     \
	    if (!ALLOW_POINTERS) {                                              \
	        type_error(L, idx, #TYPE, 0, NULL);                             \
	    }                                                                   \
	    if (!get_cfunction_address(L, idx, &f)) {                           \
	        type_error(L, idx, #TYPE, 0, NULL);                             \
	    }                                                                   \
	    ret = (TYPE) (intptr_t) f;                                          \
	    break;                                                              \
	                                                                        \
	case LUA_TUSERDATA:                                                     \
	    p = to_cdata(L, idx, &ct);                                          \
	                                                                        \
	    if (ct.type == INVALID_TYPE) {                                      \
	        if (!ALLOW_POINTERS) {                                          \
	            type_error(L, idx, #TYPE, 0, NULL);                         \
	        }                                                               \
	        ret = (TYPE) (intptr_t) userdata_toptr(L, idx);                 \
	    } else if (ct.pointers || ct.type == STRUCT_TYPE || ct.type == UNION_TYPE) {\
	        if (!ALLOW_POINTERS) {                                          \
	            type_error(L, idx, #TYPE, 0, NULL);                         \
	        }                                                               \
	        ret = (TYPE) (intptr_t) p;                                      \
	    } else if (ct.type == COMPLEX_DOUBLE_TYPE) {                        \
	        ret = (TYPE) creal(*(complex_double*) p);                       \
	    } else if (ct.type == COMPLEX_FLOAT_TYPE) {                         \
	        ret = (TYPE) crealf(*(complex_float*) p);                       \
	    } else if (ct.type == DOUBLE_TYPE) {                                \
	        ret = (TYPE) *(double*) p;                                      \
	    } else if (ct.type == FLOAT_TYPE) {                                 \
	        ret = (TYPE) *(float*) p;                                       \
	    } else {                                                            \
	        ret = check_intptr(L, idx, p, &ct);                             \
	    }                                                                   \
	    lua_pop(L, 1);                                                      \
	    break;                                                              \
	                                                                        \
	case LUA_TNIL:                                                          \
	    ret = (TYPE) 0;                                                     \
	    break;                                                              \
	                                                                        \
	default:                                                                \
	    type_error(L, idx, #TYPE, 0, NULL);                                 \
	}                                                                       \

int64_t cast_int64(lua_State* L, int idx, int is_cast) {
	TO_NUMBER(int64_t, is_cast, lua_tointeger); return ret;
}

uint64_t cast_uint64(lua_State* L, int idx, int is_cast) {
	TO_NUMBER(uint64_t, is_cast, lua_tointeger); return ret;
}

int32_t check_int32(lua_State* L, int idx) {
	return (int32_t) cast_int64(L, idx, 0);
}

uint32_t check_uint32(lua_State* L, int idx) {
	return (uint32_t) cast_uint64(L, idx, 0);
}

int64_t check_int64(lua_State* L, int idx) {
	return cast_int64(L, idx, 0);
}

uint64_t check_uint64(lua_State* L, int idx) {
	return cast_uint64(L, idx, 0);
}

double check_double(lua_State* L, int idx) {
	TO_NUMBER(double, 0, lua_tonumber); return ret;
}

float check_float(lua_State* L, int idx) {
	TO_NUMBER(double, 0, lua_tonumber); return ret;
}

uintptr_t check_uintptr(lua_State* L, int idx) {
	TO_NUMBER(uintptr_t, 1, lua_tointeger); return ret;
}

complex_double check_complex_double(lua_State* L, int idx)
{
	double real = 0, imag = 0;
	void* p;
	CType ct;

	switch (lua_type(L, idx)) {
	case LUA_TNUMBER:
		real = (double) lua_tonumber(L, idx);
		break;
	case LUA_TTABLE:
		lua_rawgeti(L, idx, 1);
		real = check_double(L, -1);
		lua_pop(L, 1);

		lua_rawgeti(L, idx, 2);
		if (lua_isnil(L, -1)) {
			imag = real;
		}  else {
			imag = check_double(L, -1);
		}
		lua_pop(L, 1);
		break;
	case LUA_TUSERDATA:
		p = to_cdata(L, idx, &ct);
		if (ct.type == COMPLEX_DOUBLE_TYPE) {
			real = creal(*(complex_double*) p);
			imag = cimag(*(complex_double*) p);
		} else if (ct.type == COMPLEX_FLOAT_TYPE) {
			real = crealf(*(complex_float*) p);
			imag = cimagf(*(complex_float*) p);
		} else if (ct.type == DOUBLE_TYPE) {
			real = *(double*) p;
		} else if (ct.type == FLOAT_TYPE) {
			real = *(float*) p;
		} else {
			real = check_intptr(L, idx, p, &ct);
		}
		lua_pop(L, 1);
		break;

	default:
		type_error(L, idx, "complex", 0, NULL);
	}

	return mk_complex_double(real, imag);
}

complex_float check_complex_float(lua_State* L, int idx)
{
	complex_double d = check_complex_double(L, idx);
	return mk_complex_float(creal(d), cimag(d));
}

static size_t unpack_vararg(lua_State* L, int i, char* to)
{
	void* p;
	CType ct;

	switch (lua_type(L, i)) {
	case LUA_TBOOLEAN:
		*(int*) to = lua_toboolean(L, i);
		return sizeof(int);

	case LUA_TNUMBER:
		*(double*) to = lua_tonumber(L, i); // TODO in Lua 5.3: lua_tointeger sometimes should be here
		return sizeof(double);

	case LUA_TSTRING:
		*(const char**) to = lua_tostring(L, i);
		return sizeof(const char*);

	case LUA_TLIGHTUSERDATA:
		*(void**) to = lua_touserdata(L, i);
		return sizeof(void*);

	case LUA_TNIL:
		*(void**) to = NULL;
		return sizeof(void*);

	case LUA_TUSERDATA:
		p = to_cdata(L, i, &ct);
		lua_pop(L, 1);

		if (ct.type == INVALID_TYPE) {
			*(void**) to = userdata_toptr(L, i);
			return sizeof(void*);

		} else if (ct.pointers || ct.type == INTPTR_TYPE) {
			*(void**) to = p;
			return sizeof(void*);

		} else if (ct.type == INT32_TYPE) {
			*(int32_t*) to = *(int32_t*) p;
			return sizeof(int32_t);

		} else if (ct.type == INT64_TYPE) {
			*(int64_t*) to = *(int64_t*) p;
			return sizeof(int64_t);
		}
		break;
	default:
		break;
	}

	type_error(L, i, "vararg", 0, NULL);	// never returns
	return 0;
}

void unpack_varargs_stack(lua_State* L, int first, int last, char* to)
{
	int i;

	for (i = first; i <= last; i++) {
		to += unpack_vararg(L, i, to);
	}
}

void unpack_varargs_stack_skip(lua_State* L, int first, int last, int ints_to_skip, int floats_to_skip, char* to)
{
	int i;

	for (i = first; i <= last; i++) {
		int type = lua_type(L, i);

		if (type == LUA_TNUMBER && --floats_to_skip >= 0) {
			continue;
		} else if (type != LUA_TNUMBER && --ints_to_skip >= 0) {
			continue;
		}

		to += unpack_vararg(L, i, to);
	}
}

void unpack_varargs_float(lua_State* L, int first, int last, int max, char* to)
{
	int i;

	for (i = first; i <= last && max > 0; i++) {
		if (lua_type(L, i) == LUA_TNUMBER) {
			unpack_vararg(L, i, to);
			to += sizeof(double);
			max--;
		}
	}
}

void unpack_varargs_int(lua_State* L, int first, int last, int max, char* to)
{
	int i;

	for (i = first; i <= last && max > 0; i++) {
		if (lua_type(L, i) != LUA_TNUMBER) {
			unpack_vararg(L, i, to);
			to += sizeof(void*);
			max--;
		}
	}
}

void unpack_varargs_reg(lua_State* L, int first, int last, char* to)
{
	int i;

	for (i = first; i <= last; i++) {
		unpack_vararg(L, i, to);
		to += sizeof(double);
	}
}

/* to_enum tries to convert a value at idx to the enum type indicated by to_ct
 * and uv to_usr. For strings this means it will do a string lookup for the
 * enum type. It leaves the stack unchanged. Will throw an error if the type
 * at idx can't be conerted.
 */
int32_t check_enum(lua_State* L, int idx, int to_usr, const CType* to_ct) {
	int32_t ret;

	switch (lua_type(L, idx)) {
	case LUA_TUSERDATA:
		return check_int32(L, idx);

	case LUA_TNIL:
		return (int32_t) 0;

	case LUA_TNUMBER:
		return (int32_t) lua_tointeger(L, idx);

	case LUA_TSTRING:
		// lookup string in to_usr to find value
		to_usr = lua_absindex(L, to_usr);
		lua_pushvalue(L, idx);
		lua_rawget(L, to_usr);

		if (!lua_isnil(L, -1)) {
			ret = (int32_t) lua_tointeger(L, -1);
			lua_pop(L, 1);
			return ret;
		}

		break;
	default:
		break;
	}

	type_error(L, idx, NULL, to_usr, to_ct);	// never returns
	return 0;
}

/* to_pointer tries converts a value at idx to a pointer. It fills out ct and
 * pushes the uv of the found type. It will throw a lua error if it can not
 * convert the value to a pointer. */
static void* check_pointer(lua_State* L, int idx, CType* ct)
{
	void* p;
	memset(ct, 0, sizeof(*ct));
	idx = lua_absindex(L, idx);

	switch (lua_type(L, idx)) {
	case LUA_TNIL:
		ct->type = VOID_TYPE;
		ct->pointers = 1;
		ct->is_null = 1;
		lua_pushnil(L);
		return NULL;

	case LUA_TNUMBER:
		ct->type = INTPTR_TYPE;
		ct->is_unsigned = 1;
		ct->pointers = 0;
		lua_pushnil(L);
		return (void*) (uintptr_t) lua_tonumber(L, idx); // TODO in Lua 5.3: maybe change to lua_tointeger

	case LUA_TLIGHTUSERDATA:
		ct->type = VOID_TYPE;
		ct->pointers = 1;
		lua_pushnil(L);
		return lua_touserdata(L, idx);

	case LUA_TSTRING:
		ct->type = INT8_TYPE;
		ct->pointers = 1;
		ct->is_unsigned = IS_CHAR_UNSIGNED;
		ct->is_array = 1;
		ct->base_size = 1;
		ct->const_mask = 2;
		lua_pushnil(L);
		return (void*) lua_tolstring(L, idx, &ct->array_size);

	case LUA_TUSERDATA:
		p = to_cdata(L, idx, ct);

		if (ct->type == INVALID_TYPE) {
			/* some other type of user data */
			ct->type = VOID_TYPE;
			ct->pointers = 1;
			return userdata_toptr(L, idx);
		} else if (ct->type == STRUCT_TYPE || ct->type == UNION_TYPE) {
			return p;
		} else {
			return (void*) (intptr_t) check_intptr(L, idx, p, ct);
		}
		break;
	}

	type_error(L, idx, "pointer", 0, NULL);
	return NULL;
}

static int is_void_ptr(const CType* ct)
{
	return ct->type == VOID_TYPE
		&& ct->pointers == 1;
}

static int is_same_type(lua_State* L, int usr1, int usr2, const CType* t1, const CType* t2)
{
	if (t1->type != t2->type) {
		return 0;
	}

#if LUA_VERSION_NUM == 501
	if (lua_isnil(L, usr1) != lua_isnil(L, usr2)) {
		int ret;
		usr1 = lua_absindex(L, usr1);
		usr2 = lua_absindex(L, usr2);
		pushRegistry(L, &niluv_key);

		ret = lua_rawequal(L, usr1, -1)
			|| lua_rawequal(L, usr2, -1);

		lua_pop(L, 1);

		if (ret) {
			return 1;
		}
	}
#endif

	return lua_rawequal(L, usr1, usr2);
}

/* to_typed_pointer converts a value at idx to a type tt with target uv to_usr
 * checking all types. May push a temporary value so that it can create
 * structs on the fly. */
void * check_typed_pointer(
	lua_State * L,
	int idx,
	int to_usr,
	const CType * tt
) {
	CType ft;
	void* p;
	to_usr = lua_absindex(L, to_usr);
	idx = lua_absindex(L, idx);

	if (tt->pointers == 1 && (tt->type == STRUCT_TYPE || tt->type == UNION_TYPE) && lua_type(L, idx) == LUA_TTABLE) {
		/* need to construct a struct of the target type */
		CType ct = *tt;
		ct.pointers = ct.is_array = 0;
		p = push_cdata(L, to_usr, &ct);
		set_struct(L, idx, p, to_usr, &ct, 1);
		return p;
	}

	p = check_pointer(L, idx, &ft);

	if (tt->pointers == 1 && ft.pointers == 0 && (ft.type == STRUCT_TYPE || ft.type == UNION_TYPE)) {
		/* auto dereference structs */
		ft.pointers = 1;
		ft.const_mask <<= 1;
	}

	if (is_void_ptr(tt)) {
		/* any pointer can convert to void* */
		return p;

	} else if (is_void_ptr(&ft) && (ft.pointers || ft.is_reference)) {
		/* void* can convert to any pointer */
		return p;

	} else if (ft.is_null) {
		/* NULL can convert to any pointer */
		return p;

	} else if (!is_same_type(L, to_usr, -1, tt, &ft)) {
		/* the base type is different */
		type_error(L, idx, NULL, to_usr, tt);

	} else if (tt->pointers != ft.pointers) {
		type_error(L, idx, NULL, to_usr, tt);

#if 0	// this is getting hit for implicit casting of Lua strings to char[]'s
		// so rather than sort out how to just copy the const-ness across for Lua strings only
		// I'll just disable it.
	} else if (ft.const_mask & ~tt->const_mask) {
		/* for every const in from it must be in to, there are further rules
		 * for const casting (see the c++ spec), but they are hard to test
		 * quickly */
		type_error(L, idx, NULL, to_usr, tt);
#endif
	}

	return p;
}

/*
Gets the address of the wrapped C function for the lua function value at idx.
Returns 1 if it exists; otherwise returns 0 and nothing is pushed.
Leaves the stack.

Wait, is this retrieving what is stored in `push_callback()`?
... which set the lua_CFunction's last-upvalue to userdata<CFunction[2]> ?
IT WOULD HELP IT IF SOMEONE WOULD HAVE NOTED THAT.
*/
static int get_cfunction_address(
	lua_State* L,
	int idx,
	CFunction* addr
) {						// stack: ...
//printf("get_cfunction_address BEGIN top=%d\n", lua_gettop(L));
	if (!lua_isfunction(L, idx)) return 0;

	// set n to the next free upvalue index
	int n = 2;
	while (lua_getupvalue(L, idx, n)) {
		lua_pop(L, 1);
		n++;
	}

	// gets the n-1'th (suprema key) upvalue
	// Why?  Why would there be any more than one callbacks stored as upvalues of the CFunction cdata?
	if (!lua_getupvalue(L, idx, n - 1)) {
		return 0;						// stack: ...
	}									// stack: ..., up = stack[idx]'s upvalue[n-1]

	if (!lua_isuserdata(L, -1) || !lua_getmetatable(L, -1)) {
		lua_pop(L, 1);					// stack: ...
		return 0;
	}									// stack: ..., up, mt = getmetatable(up)

	pushRegistry(L, &callback_mt_key);	// stack: ..., up, mt, registry[&callback_mt_key]
	if (!lua_rawequal(L, -1, -2)) {
		lua_pop(L, 3);					// stack: ...
		return 0;
	}									// stack: ..., up, mt, registry[&callback_mt_key]

	CFunction * f = (CFunction *)lua_touserdata(L, -3);	// stack: ..., up, mt, registry[&callback_mt_key]
	*addr = f[1];
	lua_pop(L, 3);						// stack: ...
//printf("get_cfunction_address END top=%d\n", lua_gettop(L));
	return 1;
}

/* to_cfunction converts a value at idx with usr table at to_usr and type tt
 * into a function. Leaves the stack unchanged. */
static CFunction check_cfunction(lua_State* L, int idx, int to_usr, const CType* tt, int check_pointers)
{
	void* p;
	CType ft;
	CFunction f;
	int top = lua_gettop(L);

	idx = lua_absindex(L, idx);
	to_usr = lua_absindex(L, to_usr);

	switch (lua_type(L, idx)) {
	case LUA_TFUNCTION:
		if (get_cfunction_address(L, idx, &f)) {
			return f;
		}

		// Function cdatas are pinned and must be manually cleaned up by calling func:free().
		pushRegistry(L, &callbacks_key);
		f = compile_callback(L, idx, to_usr, tt);
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
		lua_pop(L, 1); /* callbacks tbl */
		return f;

	case LUA_TNIL:
		return NULL;

	case LUA_TLIGHTUSERDATA:
		if (check_pointers) {
			goto err;
		} else {
			return (CFunction) lua_touserdata(L, idx);
		}

	case LUA_TUSERDATA:
		p = to_cdata(L, idx, &ft);
		assert(lua_gettop(L) == top + 1);

		if (ft.type == INVALID_TYPE) {
			if (check_pointers) {
				goto err;
			} else {
				lua_pop(L, 1);
				return (CFunction) lua_touserdata(L, idx);
			}

		} else if (ft.is_null) {
			lua_pop(L, 1);
			return NULL;

		} else if (!check_pointers && (ft.pointers || ft.type == INTPTR_TYPE)) {
			lua_pop(L, 1);
			return (CFunction) *(void**) p;

		} else if (ft.type != FUNCTION_PTR_TYPE) {
			goto err;

		} else if (!check_pointers) {
			lua_pop(L, 1);
			return *(CFunction*) p;

		} else if (ft.calling_convention != tt->calling_convention) {
			goto err;

		} else if (!is_same_type(L, -1, to_usr, &ft, tt)) {
			goto err;

		} else {
			lua_pop(L, 1);
			return *(CFunction*) p;
		}

	default:
		goto err;
	}

err:
	type_error(L, idx, NULL, to_usr, tt);
	return NULL;
}

/* to_type_cfunction converts a value at idx with uv at to_usr and type tt to
 * a CFunction. Leaves the stack unchanged. */
CFunction check_typed_cfunction(lua_State* L, int idx, int to_usr, const CType* tt) {
	return check_cfunction(L, idx, to_usr, tt, 1);
}

static void set_value(lua_State* L, int idx, void* to, int to_usr, const CType* tt, int check_pointers);

static void set_array(lua_State* L, int idx, void* to, int to_usr, const CType* tt, int check_pointers)
{
	size_t i, sz, esz;
	CType et;

	idx = lua_absindex(L, idx);
	to_usr = lua_absindex(L, to_usr);

	switch (lua_type(L, idx)) {
	case LUA_TSTRING:
		if (tt->pointers == 1 && tt->type == INT8_TYPE) {
			const char* str = lua_tolstring(L, idx, &sz);

			if (!tt->is_variable_array && sz >= tt->array_size) {
				memcpy(to, str, tt->array_size);
			} else {
				/* include nul terminator */
				memcpy(to, str, sz+1);
			}
		} else {
			goto err;
		}
		break;

	case LUA_TTABLE:
		et = *tt;
		et.pointers--;
		et.const_mask >>= 1;
		et.is_array = 0;
		esz = et.pointers ? sizeof(void*) : et.base_size;

		lua_rawgeti(L, idx, 2);

		if (tt->is_variable_array) {
			/* we have no idea how big the array is, so set values based off
			 * how many items were given to us */
			lua_pop(L, 1);
			for (i = 0; i < lua_rawlen(L, idx); i++) {
				lua_rawgeti(L, idx, (int) i + 1);
				set_value(L, -1, (char*) to + esz * i, to_usr, &et, check_pointers);
				lua_pop(L, 1);
			}

		} else if (lua_isnil(L, -1)) {
			/* there is no second element, so we set the whole array to the
			 * first element (or nil - ie 0) if there is no first element) */
			lua_pop(L, 1);
			lua_rawgeti(L, idx, 1);

			if (lua_isnil(L, -1)) {
				memset(to, 0, ctype_size(L, tt));
			} else {
				/* if its still variable we have no idea how many values to set */
				for (i = 0; i < tt->array_size; i++) {
					set_value(L, -1, (char*) to + esz * i, to_usr, &et, check_pointers);
				}
			}

			lua_pop(L, 1);

		} else {
			/* there is a second element, so we set each element using the
			 * equiv index in the table initializer */
			lua_pop(L, 1);
			for (i = 0; i < tt->array_size; i++) {
				lua_rawgeti(L, idx, (int) (i+1));

				if (lua_isnil(L, -1)) {
					/* we've hit the end of the values provided in the
					 * initializer, so memset the rest to zero */
					lua_pop(L, 1);
					memset((char*) to + esz * i, 0, (tt->array_size - i) * esz);
					break;

				} else {
					set_value(L, -1, (char*) to + esz * i, to_usr, &et, check_pointers);
					lua_pop(L, 1);
				}
			}
		}
		break;

	default:
		goto err;
	}

	return;

err:
	type_error(L, idx, NULL, to_usr, tt);
}

/* pops the member key from the stack, leaves the member user value on the
 * stack. Returns the member offset. Returns -ve if the member can not be
 * found. */
static ptrdiff_t get_member(lua_State* L, int usr, const CType* ct, CType* mt)
{
	ptrdiff_t off;
	lua_rawget(L, usr);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	*mt = *(const CType*) lua_touserdata(L, -1);
	lua_getuservalue(L, -1);
	lua_replace(L, -2);

	if (mt->is_variable_array && ct->variable_size_known) {
		/* eg char mbr[?] */
		size_t sz = (mt->pointers > 1) ? sizeof(void*) : mt->base_size;
		assert(ct->is_variable_struct && mt->is_array);
		mt->array_size = ct->variable_increment / sz;
		mt->is_variable_array = 0;

	} else if (mt->is_variable_struct && ct->variable_size_known) {
		/* eg struct {char a; char b[?]} mbr; */
		assert(ct->is_variable_struct);
		mt->variable_size_known = 1;
		mt->variable_increment = ct->variable_increment;
	}

	off = mt->offset;
	mt->offset = 0;
	return off;
}

void set_struct(lua_State* L, int idx, void* to, int to_usr, const CType* tt, int check_pointers)
{
	int have_first = 0;
	int have_other = 0;
	CType mt;
	void* p;

	to_usr = lua_absindex(L, to_usr);
	idx = lua_absindex(L, idx);

	switch (lua_type(L, idx)) {
	case LUA_TTABLE:
		/* match up to the members based off the table initializers key - this
		 * will match both numbered and named members in the user table
		 * we need a special case for when no entries in the initializer -
		 * zero initialize the c struct, and only one entry in the initializer
		 * - set all members to this value */
		memset(to, 0, ctype_size(L, tt));
		lua_pushnil(L);
		while (lua_next(L, idx)) {
			ptrdiff_t off;

			if (!have_first && lua_tonumber(L, -2) == 1 && lua_tonumber(L, -1) != 0) {
				have_first = 1;
			} else if (!have_other && (lua_type(L, -2) != LUA_TNUMBER || lua_tonumber(L, -2) != 1)) {
				have_other = 1;
			}

			lua_pushvalue(L, -2);
			off = get_member(L, to_usr, tt, &mt);
			assert(off >= 0);
			set_value(L, -2, (char*) to + off, -1, &mt, check_pointers);

			/* initializer value, mt usr */
			lua_pop(L, 2);
		}

		/* if we only had a single non zero value then initialize all members to that value */
		if (!have_other && have_first && tt->type != UNION_TYPE) {
			size_t i, sz;
			ptrdiff_t off;
			lua_rawgeti(L, idx, 1);
			sz = lua_rawlen(L, to_usr);

			for (i = 2; i < sz; i++) {
				lua_pushinteger(L, i);
				off = get_member(L, to_usr, tt, &mt);
				assert(off >= 0);
				set_value(L, -2, (char*) to + off, -1, &mt, check_pointers);
				lua_pop(L, 1); /* mt usr */
			}

			lua_pop(L, 1); /* initializer table */
		}
		break;

	case LUA_TUSERDATA:
		if (check_pointers) {
			p = check_typed_pointer(L, idx, to_usr, tt);
		} else {
			CType ct;
			p = check_pointer(L, idx, &ct);
		}
		memcpy(to, p, tt->base_size);
		lua_pop(L, 1);
		break;

	default:
		goto err;
	}

	return;

err:
	type_error(L, idx, NULL, to_usr, tt);
}

static void set_value(lua_State* L, int idx, void* to, int to_usr, const CType* tt, int check_pointers)
{
	int top = lua_gettop(L);

	if (tt->is_array) {
		set_array(L, idx, to, to_usr, tt, check_pointers);

	} else if (tt->pointers || tt->is_reference) {
		union {
			uint8_t c[sizeof(void*)];
			void* p;
		} u;

		if (lua_istable(L, idx)) {
			luaL_error(L, "Can't set a pointer member to a struct that's about to be freed");
		}

		if (check_pointers) {
			u.p = check_typed_pointer(L, idx, to_usr, tt);
		} else {
			CType ct;
			u.p = check_pointer(L, idx, &ct);
		}

#ifndef ALLOW_MISALIGNED_ACCESS
		if ((uintptr_t) to & PTR_ALIGN_MASK) {
			memcpy(to, u.c, sizeof(void*));
		} else
#endif
		{
			*(void**) to = u.p;
		}

		lua_pop(L, 1);

	} else if (tt->is_bitfield) {

		uint64_t hi_mask = UINT64_C(0) - (UINT64_C(1) << (tt->bit_offset + tt->bit_size));
		uint64_t low_mask = (UINT64_C(1) << tt->bit_offset) - UINT64_C(1);
		uint64_t val = check_uint64(L, idx);
		val &= (UINT64_C(1) << tt->bit_size) - 1;
		val <<= tt->bit_offset;
		*(uint64_t*) to = val | (*(uint64_t*) to & (hi_mask | low_mask));

	} else if (tt->type == STRUCT_TYPE || tt->type == UNION_TYPE) {
		set_struct(L, idx, to, to_usr, tt, check_pointers);

	} else {

#ifndef ALLOW_MISALIGNED_ACCESS
		union {
			uint8_t c[8];
			_Bool b;
			uint64_t u64;
			float f;
			double d;
			CFunction func;
		} misalign;

		void* origto = to;

		if ((uintptr_t) origto & (tt->base_size - 1)) {
			to = misalign.c;
		}
#endif

		switch (tt->type) {
		case BOOL_TYPE:
			*(_Bool*) to = (cast_int64(L, idx, !check_pointers) != 0);
			break;
		case INT8_TYPE:
			if (tt->is_unsigned) {
				*(uint8_t*) to = (uint8_t) cast_uint64(L, idx, !check_pointers);
			} else {
				*(int8_t*) to = (int8_t) cast_int64(L, idx, !check_pointers);
			}
			break;
		case INT16_TYPE:
			if (tt->is_unsigned) {
				*(uint16_t*) to = (uint16_t) cast_uint64(L, idx, !check_pointers);
			} else {
				*(int16_t*) to = (int16_t) cast_int64(L, idx, !check_pointers);
			}
			break;
		case INT32_TYPE:
			if (tt->is_unsigned) {
				*(uint32_t*) to = (uint32_t) cast_uint64(L, idx, !check_pointers);
			} else {
				*(int32_t*) to = (int32_t) cast_int64(L, idx, !check_pointers);
			}
			break;
		case INT64_TYPE:
			if (tt->is_unsigned) {
				*(uint64_t*) to = cast_uint64(L, idx, !check_pointers);
			} else {
				*(int64_t*) to = cast_int64(L, idx, !check_pointers);
			}
			break;
		case FLOAT_TYPE:
			*(float*) to = (float) check_double(L, idx);
			break;
		case DOUBLE_TYPE:
			*(double*) to = check_double(L, idx);
			break;
		case COMPLEX_FLOAT_TYPE:
			*(complex_float*) to = check_complex_float(L, idx);
			break;
		case COMPLEX_DOUBLE_TYPE:
			*(complex_double*) to = check_complex_double(L, idx);
			break;
		case INTPTR_TYPE:
			*(uintptr_t*) to = check_uintptr(L, idx);
			break;
		case ENUM_TYPE:
			*(int32_t*) to = check_enum(L, idx, to_usr, tt);
			break;
		case FUNCTION_PTR_TYPE:
			*(CFunction*) to = check_cfunction(L, idx, to_usr, tt, check_pointers);
			break;
		default:
			goto err;
		}

#ifndef ALLOW_MISALIGNED_ACCESS
		if ((uintptr_t) origto & (tt->base_size - 1)) {
			memcpy(origto, misalign.c, tt->base_size);
		}
#endif
	}

	assert(lua_gettop(L) == top);
	return;
err:
	type_error(L, idx, NULL, to_usr, tt);
}

/*
if arg 1 is a ctype, returns the ctype
if arg 1 is a string...
	then args 2..n *must be* ctypes
	and the # of args *must* match the # of variables in the type string
*/
static int ffi_typeof(lua_State* L) {
	CType ct;
	check_ctype(L, 1, &ct, 2); // 1st arg is string, rest of args are type params
	push_ctype(L, -1, &ct);
	return 1;
}

static void setmintop(lua_State* L, int idx)
{
	if (lua_gettop(L) < idx) {
		lua_settop(L, idx);
	}
}

// warning: in the case that it finds an array size, it removes that index
static void get_variable_array_size(lua_State* L, int idx, CType* ct)
{
	/* we only care about the variable buisness for the variable array
	 * directly ie ffi.new('char[?]') or the struct that contains the variable
	 * array ffi.new('struct {char v[?]}'). A pointer to the struct doesn't
	 * care about the variable size (it treats it as a zero sized array). */

	if (ct->is_variable_array) {
		assert(ct->is_array);
		ct->array_size = (size_t) luaL_checknumber(L, idx);
		ct->is_variable_array = 0;
		lua_remove(L, idx);

	} else if (ct->is_variable_struct && !ct->variable_size_known) {
		assert(ct->type == STRUCT_TYPE && !ct->is_array);
		ct->variable_increment *= (size_t) luaL_checknumber(L, idx);
		ct->variable_size_known = 1;
		lua_remove(L, idx);
	}
}

static int is_scalar(CType* ct)
{
	int type = ct->type;
	if (ct->pointers || ct->is_reference) {
		return !ct->is_array;
	}
	return type != STRUCT_TYPE && type != UNION_TYPE && !IS_COMPLEX(type);
}

static int should_pack(lua_State *L, int ct_usr, CType* ct, int idx)
{
	CType argt;
	ct_usr = lua_absindex(L, ct_usr);

	if (IS_COMPLEX(ct->type)) {
		return 0;
	}

	switch (lua_type(L, idx)) {
	case LUA_TTABLE:
		return 0;
	case LUA_TSTRING:
		return ct->type == STRUCT_TYPE;
	case LUA_TUSERDATA:
		// don't pack if the argument is a cdata with the same type
		to_cdata(L, idx, &argt);
		int same = is_same_type(L, ct_usr, -1, ct, &argt);
		lua_pop(L, 1);
		return !same;
	}
	return 1;
}

/*
Incoming stack is ctype (or string) and args
*/
static int do_new(
	lua_State * L,
	int is_cast
) {													// stack: typedesc, args...
	CType ct;
	check_ctype(L, 1, &ct, 0);						// stack: typedesc, args..., typedesc's CType's uservalue[1]

	// don't push a callback when we have a c function, as cb:set needs a
	// compiled callback from a lua function to work
	if (!ct.pointers &&
		ct.type == FUNCTION_PTR_TYPE &&
		(lua_isnil(L, 2) || lua_isfunction(L, 2))
	) {
		// Get the bound C function if this is a ffi lua function
		CFunction func;
		if (get_cfunction_address(L, 2, &func)) {	// stack: typedesc, args..., typedesc's CType's uservalue[1]
			void * p = push_cdata(L, -1, &ct);		// stack: typedesc, args..., typedesc's CType's uservalue[1], userdata of CData of CType ct
			*(CFunction *)p = func;
			return 1;								// return the userdata of CData of CType ct
		}
		// Function cdatas are pinned and must be manually cleaned up by calling func:free()
													// stack: typedesc, args..., typedesc's CType's uservalue[1]
//printf("do_new compile_callback\n");
		CFunction closureCDataPtr = compile_callback(L, 2, -1, &ct);	// stack: typedesc, args..., typedesc's CType's uservalue[1], CData of libffi-closure
#if 0
printf("do_new got closureCDataPtr=%p\n", closureCDataPtr);
printf("can I call it?\n");
typedef int (*TEMPFUNC)(char const *);
TEMPFUNC f = (TEMPFUNC)closureCDataPtr;
int cfuncResult = f("testing from luaffifb");
printf("cfuncResult %d\n", cfuncResult);

exit(1);	//done for now
#endif
#if 0	// TODO maybe or maybe not
		pushRegistry(L, &callbacks_key);
		lua_pushvalue(L, -2);
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
		lua_pop(L, 1); // callbacks tbl
#endif
		return 1;
	}

	// stack: typedesc, args..., typedesc's CType's uservalue[1]

	// this removes the vararg argument if its needed, and errors if its invalid
	if (!is_cast) {
		get_variable_array_size(L, 2, &ct);
	}

	// stack: typedesc, args..., typedesc's CType's uservalue[1]

	void * p = push_cdata(L, -1, &ct);		// stack: typedesc, args..., typedesc's CType's uservalue[1], CData userdata with uservalue[1] set to ct CType's userdata's uservalue[1]

	// if the user mt has a __gc function then call ffi.gc on this value
	if (push_user_mt(L, -2, &ct)) {
		pushRegistry(L, &gc_key);
		lua_pushvalue(L, -3);

		// user_mt.__gc
		lua_pushliteral(L, "__gc");
		lua_rawget(L, -4);

		lua_rawset(L, -3); // gc_upval[cdata] = user_mt.__gc
		lua_pop(L, 2); // user_mt and gc_upval
	}

	/* stack is:
	 * ctype arg
	 * ctor args ... 0+
	 * ctype usr
	 * cdata
	 */

	int cargs = lua_gettop(L) - 3;

	if (cargs == 0) {
		return 1;
	}

	int scalar = is_scalar(&ct);
	if (scalar && cargs > 1) {
		return luaL_error(L, "too many initializers");
	}

	if (cargs > 1 || (!scalar && should_pack(L, -2, &ct, 2))) {
		lua_createtable(L, cargs, 0);
		lua_replace(L, 1);
		for (int i = 1; i <= cargs; i++) {
			lua_pushvalue(L, i + 1);
			lua_rawseti(L, 1, i);
		}
		assert(lua_gettop(L) == cargs + 3);
		set_value(L, 1, p, -2, &ct, !is_cast);	// cast <-> don't check pointers
		return 1;
	}

	set_value(L, 2, p, -2, &ct, !is_cast);	// cast <-> don't check pointers
	return 1;
}

static int ffi_new(lua_State* L) { return do_new(L, 0); }
static int ffi_cast(lua_State* L) { return do_new(L, 1); }
static int ctype_new(lua_State* L) { return do_new(L, 0); }

static int ctype_call(lua_State* L) {
	CType ct;
	int top = lua_gettop(L);

	check_ctype(L, 1, &ct, 0);

	if (push_user_mt(L, -1, &ct)) {
		lua_pushstring(L, "__new");
		lua_rawget(L, -2);
		if (!lua_isnil(L, -1)) {
			lua_insert(L, 1); // function at bottom of stack under args
			lua_pop(L, 2);
			lua_call(L, top, 1);
			return 1;
		}
		lua_pop(L, 2);
	}
	lua_pop(L, 1);

	assert(lua_gettop(L) == top);
	return do_new(L, 0);
}

static int ffi_sizeof(lua_State* L)
{
	CType ct;
	check_ctype(L, 1, &ct, 0);
	get_variable_array_size(L, 2, &ct);
	lua_pushinteger(L, ctype_size(L, &ct));
	return 1;
}

static int ffi_alignof(lua_State* L)
{
	CType ct, mt;
	lua_settop(L, 2);
	check_ctype(L, 1, &ct, 0);

	/* if no member is specified then we return the alignment of the type */
	if (lua_isnil(L, 2)) {
		lua_pushinteger(L, ct.align_mask + 1);
		return 1;
	}

	/* get the alignment of the member */
	lua_pushvalue(L, 2);
	if (get_member(L, -2, &ct, &mt) < 0) {
		push_type_name(L, 3, &ct);
		return luaL_error(L, "type %s has no member %s", lua_tostring(L, -1), lua_tostring(L, 2));
	}

	lua_pushinteger(L, mt.align_mask + 1);
	return 1;
}

static int ffi_offsetof(lua_State* L)
{
	ptrdiff_t off;
	CType ct, mt;
	lua_settop(L, 2);
	check_ctype(L, 1, &ct, 0);

	lua_pushvalue(L, 2);
	off = get_member(L, -2, &ct, &mt); /* this replaces the member key at -1 with the mbr usr value */
	if (off < 0) {
		push_type_name(L, 3, &ct);
		return luaL_error(L, "type %s has no member %s", lua_tostring(L, -1), lua_tostring(L, 2));
	}

	lua_pushinteger(L, off);

	if (!mt.is_bitfield) {
		return 1;
	}

	lua_pushinteger(L, mt.bit_offset);
	lua_pushinteger(L, mt.bit_size);
	return 3;
}

static int ffi_istype(
	lua_State* L
) {							// stack: typedesc, obj
	CType tt;
	check_ctype(L, 1, &tt, 0);	// stack: typedesc, obj, ttuv = typedesc's CType userdata's uservalue[1]

	CType ft;
	to_cdata(L, 2, &ft);	// stack: typedesc, obj, ttuv, objuv = obj's CType userdata's uservalue[1]

	lua_pushboolean(L,
		!(ft.type == INVALID_TYPE)
		&& !(!is_same_type(L, 3, 4, &tt, &ft))
		&& !(tt.pointers != ft.pointers)
		&& !(tt.is_array != ft.is_array)
		&& !(tt.is_array && tt.array_size != ft.array_size)
		&& !(tt.calling_convention != ft.calling_convention)
	);
	return 1;
}

static int cdata_gc(lua_State* L)
{
	CType ct;
	check_cdata(L, 1, &ct);
	lua_settop(L, 1);

	// call the gc func if there is any registered
	lua_pushvalue(L, 1);
	lua_rawget(L, lua_upvalueindex(2));
	if (!lua_isnil(L, -1)) {
		lua_pushvalue(L, 1);
		lua_pcall(L, 1, 0, 0);
	}

	// unset the closure
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	return 0;
}

static int callback_free(lua_State* L)
{
	//CFunction* p = (CFunction*) lua_touserdata(L, 1);
	// FIXME: temporarily disabled to prevent SIGTRAP on exit
	// free_code(get_jit(L), L, *p);
	return 0;
}

static int cdata_free(lua_State* L)
{
	CType ct;
	CFunction* p = (CFunction*) check_cdata(L, 1, &ct);
	lua_settop(L, 1);

	/* unset the closure */
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

	if (ct.is_jitted) {
		free_code(get_jit(L), L, *p);
		*p = NULL;
	}

	return 0;
}

static int cdata_set(lua_State* L)
{
	CType ct;
	CFunction* p = (CFunction*) check_cdata(L, 1, &ct);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	if (!ct.is_jitted) {
		luaL_error(L, "can't set the function for a non-lua callback");
	}

	if (*p == NULL) {
		luaL_error(L, "can't set the function for a free'd callback");
	}

	push_func_ref(L, *p);
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, CALLBACK_FUNC_USR_IDX);

	/* remove the closure for this callback as it embeds the function pointer
	 * value */
	lua_pushvalue(L, 1);
	lua_pushboolean(L, 1);
	lua_rawset(L, lua_upvalueindex(1));

	return 0;
}

/*
This function is only for:
- CData with __call metamethods
- C function-ptrs retrieved from C API
Everything else is handled elsewhere.

stack[1] is CData
*/
static int cdata_call(
	lua_State * L
) {									// stack: obj, ...
	int top = lua_gettop(L);
	CType ct;
	CFunction * p = (CFunction *)check_cdata(L, 1, &ct);	// stack: obj, ..., objUserVal = obj's uservalue[1]

	if (push_user_mt(L, -1, &ct)) {
		// Handle CData __call metamethods:

		lua_pushliteral(L, "__call");
		lua_rawget(L, -2);

		if (!lua_isnil(L, -1)) {
			lua_insert(L, 1);
			lua_pop(L, 2); // ct_usr, user_mt
			lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
			return lua_gettop(L);
		}
	}	// stack: obj, ..., objUserVal

#if defined(CALL_WITH_LIBFFI)
	if (ct.type == FUNCTION_TYPE) {
		// Handle CData of functions from cmodule_index

		lua_pushvalue(L, 1);			// stack: obj, ..., objUserVal, obj
		lua_rawget(L, -2);				// stack: obj, ..., objUserVal, closure = objUserValue[obj]
		if (lua_tocfunction(L, -1) == callLuaToCWithLibFFI) {
			lua_remove(L, -2);			// stack: obj, ..., closure = objUserValue[obj]
			lua_replace(L, 1);			// stack: closure, ...
			lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);	// stack: closure(...)'s results...
			return lua_gettop(L);
		}
		assert(lua_type(L, -1) == LUA_TNIL);	//right? who else is using this?
	}
#endif	//CALL_WITH_LIBFFI

	if (ct.pointers || ct.type != FUNCTION_PTR_TYPE) {
		return luaL_error(L, "only function callbacks are callable");
	}

	// Handle C function-ptrs:
#if defined(CALL_WITH_LIBFFI)
	// now compile_function returns a CData so I gotta work around that

	lua_pushvalue(L, 1);					// stack: obj, ..., objUserVal, obj
	lua_rawget(L, lua_upvalueindex(1));		// stack: obj, ..., objUserVal, objUpVal = obj's upvalue[1] = function-closure
	if (!lua_isfunction(L, -1)) {			// if obj's upvalue[1] is not a lua-function ...
		lua_pop(L, 1);						// stack: obj, ..., objUserVal
		compile_function(L, *p, -1, &ct);	// stack: obj, ..., objUserVal, CData userdata of the function ... same as obj? or dif userdata representing the same CData?
		lua_rawget(L, -2);					// stack: obj, ..., objUserVal, closure = objUserVal[CData userdata of function]
		assert(lua_type(L, -1) == LUA_TFUNCTION);
		assert(lua_gettop(L) == top + 2); 	// stack: obj, ..., objUserVal, closure
		lua_replace(L, 1);					// stack: closure, ..., objUserVal
	} else {
		lua_replace(L, 1);					// stack: closure, ..., objUserVal
	}

	lua_pop(L, 1);							// stack: closure, ...


#else	// The old way:

	lua_pushvalue(L, 1);					// stack: obj, ..., objUserVal, obj
	lua_rawget(L, lua_upvalueindex(1));		// stack: obj, ..., objUserVal, objUpVal = obj's upvalue[1] = function-closure in some cases? idk when ... whenever a lua-function invokes a __call method, which is never.
	if (!lua_isfunction(L, -1)) {			// if obj's upvalue[1] is not a lua-function ...
		lua_pop(L, 1);						// stack: obj, ..., objUserVal
		compile_function(L, *p, -1, &ct);	// stack: obj, ..., objUserVal, closure = the lua_CFunction returned by compile_function

		assert(lua_gettop(L) == top + 2); 	// stack: obj, ..., objUserVal, closure

		// closures[func] = closure
		lua_pushvalue(L, 1);				// stack: obj, ..., objUserVal, closure, obj
		lua_pushvalue(L, -2);				// stack: obj, ..., objUserVal, closure, obj, closure
		lua_rawset(L, lua_upvalueindex(1));	// stack: obj, ..., objUserVal, closure;  objUserVal[obj] = closure

		lua_replace(L, 1);					// stack: closure, ..., objUserVal
	} else {
		lua_replace(L, 1);					// stack: closure, ..., objUserVal
	}

	lua_pop(L, 1);							// stack: closure, ...
#endif
	assert(lua_gettop(L) == top);

	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);	// stack: closure(...)'s results...
	return lua_gettop(L);
}

static int user_mt_key;

static int ffi_metatype(lua_State* L)
{
	CType ct;
	lua_settop(L, 2);

	check_ctype(L, 1, &ct, 0);

	if (ct.is_array) {
		return luaL_error(L, "bad argument #1 to 'metatype' (invalid C type)");
	}

	if (lua_type(L, 2) != LUA_TTABLE && lua_type(L, 2) != LUA_TNIL) {
		return luaL_argerror(L, 2, "metatable must be a table or nil");
	}

	lua_pushlightuserdata(L, &user_mt_key);
	lua_pushvalue(L, 2);
	lua_rawset(L, 3); // user[user_mt_key] = mt

	// return the passed in ctype
	push_ctype(L, 3, &ct);
	return 1;
}

/*
If the type has a user metatable then pushes it onto the stack and returns 1,
Otherwise pushes nothing and returns 0.
*/
int push_user_mt(
	lua_State * L,
	int ct_usr,
	const CType * ct
) {
	if (ct->type != STRUCT_TYPE
		&& ct->type != UNION_TYPE
		&& !IS_COMPLEX(ct->type)
	) {
		return 0;
	}

	if (!lua_istable(L, ct_usr)) {
		return 0;
	}

	ct_usr = lua_absindex(L, ct_usr);
	lua_pushlightuserdata(L, &user_mt_key);
	lua_rawget(L, ct_usr);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}

	return 1;
}

static int ffi_gc(lua_State* L)
{
	CType ct;
	lua_settop(L, 2);
	check_cdata(L, 1, &ct);

	pushRegistry(L, &gc_key);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, -3);

	/* return the cdata back */
	lua_settop(L, 1);
	return 1;
}

/* lookup_cdata_index returns the offset of the found type and user value on
 * the stack if valid. Otherwise returns -ve and doesn't touch the stack.
 */
static ptrdiff_t lookup_cdata_index(lua_State* L, int idx, int ct_usr, CType* ct)
{
	CType mt;
	ptrdiff_t off;

	ct_usr = lua_absindex(L, ct_usr);
	int type = lua_type(L, idx);

	switch (type) {
	case LUA_TNUMBER:
	case LUA_TUSERDATA:
		/* possibilities are array, pointer */

		if (!ct->pointers || is_void_ptr(ct)) {
			return -1;
		}

		// unbox cdata
		if (type == LUA_TUSERDATA) {
			if (!cdata_tointeger(L, idx, &off)) {
				return -1;
			}
		} else {
			off = lua_tointeger(L, idx);
		}

		ct->is_array = 0;
		ct->pointers--;
		ct->const_mask >>= 1;
		ct->is_reference = 0;

		lua_pushvalue(L, ct_usr);

		return (ct->pointers ? sizeof(void*) : ct->base_size) * off;

	case LUA_TSTRING:
		/* possibilities are struct/union, pointer to struct/union */

		if ((ct->type != STRUCT_TYPE && ct->type != UNION_TYPE) || ct->is_array || ct->pointers > 1) {
			return -1;
		}

		lua_pushvalue(L, idx);
		off = get_member(L, ct_usr, ct, &mt);
		if (off < 0) {
			return -1;
		}

		*ct = mt;
		return off;

	default:
		return -1;
	}
}

static int cdata_newindex(lua_State* L)
{
	CType tt;
	char* to;
	ptrdiff_t off;

	lua_settop(L, 3);

	to = (char*) check_cdata(L, 1, &tt);
	off = lookup_cdata_index(L, 2, -1, &tt);

	if (off < 0) {
		if (!push_user_mt(L, -1, &tt)) {
			goto err;
		}

		lua_pushliteral(L, "__newindex");
		lua_rawget(L, -2);

		if (lua_isnil(L, -1)) {
			goto err;
		}

		lua_insert(L, 1);
		lua_settop(L, 4);
		lua_call(L, 3, LUA_MULTRET);
		return lua_gettop(L);
	}

	if (tt.const_mask & 1) {
		return luaL_error(L, "can't set const data");
	}

	set_value(L, 3, to + off, -1, &tt, 1);
	return 0;

err:
	push_type_name(L, 4, &tt);
	return luaL_error(L, "type %s has no member %s", lua_tostring(L, -1), lua_tostring(L, 2));
}

static int cdata_index(lua_State* L)
{
	void* to;
	CType ct;
	char* data;
	ptrdiff_t off;

	lua_settop(L, 2);
	data = (char*) check_cdata(L, 1, &ct);
	assert(lua_gettop(L) == 3);

	if (!ct.pointers) {
		switch (ct.type) {
		case FUNCTION_PTR_TYPE:
			/* Callbacks use the same metatable as standard cdata values, but have set
			 * and free members. So instead of mt.__index = mt, we do the equiv here. */
			lua_getmetatable(L, 1);
			lua_pushvalue(L, 2);
			lua_rawget(L, -2);
			return 1;

			/* This provides the .re and .im virtual members */
		case COMPLEX_DOUBLE_TYPE:
		case COMPLEX_FLOAT_TYPE:
			if (!lua_isstring(L, 2)) {
				luaL_error(L, "invalid member for complex number");

			} else if (strcmp(lua_tostring(L, 2), "re") == 0) {
				lua_pushnumber(L, ct.type == COMPLEX_DOUBLE_TYPE ? creal(*(complex_double*) data) : crealf(*(complex_float*) data));

			} else if (strcmp(lua_tostring(L, 2), "im") == 0) {
				lua_pushnumber(L, ct.type == COMPLEX_DOUBLE_TYPE ? cimag(*(complex_double*) data) : cimagf(*(complex_float*) data));

			} else {
				luaL_error(L, "invalid member for complex number");
			}
			return 1;
		}
	}

	off = lookup_cdata_index(L, 2, -1, &ct);

	if (off < 0) {	// no ctype field found (right?)
		assert(lua_gettop(L) == 3);

		// if it's an array type and no field was found then error
		// don't let the array use the base type's ctype meta-info.
		if (ct.is_array) {
			goto err;
		}

		if (!push_user_mt(L, -1, &ct)) {
			goto err;
		}

		lua_pushliteral(L, "__index");
		lua_rawget(L, -2);

		if (lua_isnil(L, -1)) {
			goto err;
		}

		if (lua_istable(L, -1)) {
			lua_pushvalue(L, 2);
			lua_gettable(L, -2);
			return 1;
		}

		lua_insert(L, 1);
		lua_settop(L, 3);
		lua_call(L, 2, LUA_MULTRET);
		return lua_gettop(L);

err:
		push_type_name(L, 3, &ct);
		return luaL_error(L, "type %s has no member %s", lua_tostring(L, -1), lua_tostring(L, 2));
	}

	assert(lua_gettop(L) == 4); // ct, key, ct_usr, mbr_usr
	data += off;

	if (ct.is_array) {
		// push a reference to the array
		ct.is_reference = 1;
		to = push_cdata(L, -1, &ct);
		*(void**) to = data;
		return 1;

	} else if (ct.is_bitfield) {

		if (ct.type == INT64_TYPE) {
			CType rt;
			uint64_t val = *(uint64_t*) data;
			val >>= ct.bit_offset;
			val &= (UINT64_C(1) << ct.bit_size) - 1;

			memset(&rt, 0, sizeof(rt));
			rt.base_size = 8;
			rt.type = INT64_TYPE;
			rt.is_unsigned = 1;
			rt.is_defined = 1;

			to = push_cdata(L, 0, &rt);
			*(uint64_t*) to = val;

			return 1;

		} else if (ct.type == BOOL_TYPE) {
			uint64_t val = *(uint64_t*) data;
			lua_pushboolean(L, (int) (val & (UINT64_C(1) << ct.bit_offset)));
			return 1;

		} else {
			uint64_t val = *(uint64_t*) data;
			val >>= ct.bit_offset;
			val &= (UINT64_C(1) << ct.bit_size) - 1;
			lua_pushinteger(L, val);
			return 1;
		}

	} else if (ct.pointers) {
#ifndef ALLOW_MISALIGNED_ACCESS
		union {
			uint8_t c[8];
			void* p;
		} misalignbuf;

		if ((uintptr_t) data & PTR_ALIGN_MASK) {
			memcpy(misalignbuf.c, data, sizeof(void*));
			data = (char*)misalignbuf.c;
		}
#endif
		to = push_cdata(L, -1, &ct);
		*(void**) to = *(void**) data;
		return 1;

	} else if (ct.type == STRUCT_TYPE || ct.type == UNION_TYPE) {
		/* push a reference to the member */
		ct.is_reference = 1;
		to = push_cdata(L, -1, &ct);
		*(void**) to = data;
		return 1;

	} else if (ct.type == FUNCTION_PTR_TYPE) {
		CFunction* pf = (CFunction*) push_cdata(L, -1, &ct);
		*pf = *(CFunction*) data;
		return 1;

	} else {
#ifndef ALLOW_MISALIGNED_ACCESS
		union {
			uint8_t c[8];
			double d;
			float f;
			uint64_t u64;
		} misalignbuf;

		assert(ct.base_size <= 8);

		if ((uintptr_t) data & (ct.base_size - 1)) {
			memcpy(misalignbuf.c, data, ct.base_size);
			data = (char*)misalignbuf.c;
		}
#endif

		switch (ct.type) {
		case BOOL_TYPE:
			lua_pushboolean(L, *(_Bool*) data);
			break;
		case INT8_TYPE:
			lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint8_t*) data : (lua_Integer) *(int8_t*) data);
			break;
		case INT16_TYPE:
			lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint16_t*) data : (lua_Integer) *(int16_t*) data);
			break;
		case ENUM_TYPE:
		case INT32_TYPE:
			lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint32_t*) data : (lua_Integer) *(int32_t*) data);
			break;
		case INT64_TYPE:
			to = push_cdata(L, -1, &ct);
			*(int64_t*) to = *(int64_t*) data;
			break;
		case INTPTR_TYPE:
			to = push_cdata(L, -1, &ct);
			*(intptr_t*) to = *(intptr_t*) data;
			break;
		case FLOAT_TYPE:
			lua_pushnumber(L, *(float*) data);
			break;
		case DOUBLE_TYPE:
			lua_pushnumber(L, *(double*) data);
			break;
		case COMPLEX_DOUBLE_TYPE:
			to = push_cdata(L, -1, &ct);
			*(complex_double*) to = *(complex_double*) data;
			break;
		case COMPLEX_FLOAT_TYPE:
			to = push_cdata(L, -1, &ct);
			*(complex_float*) to = *(complex_float*) data;
			break;
		default:
			luaL_error(L, "internal error: invalid member type");
		}

		return 1;
	}
}

static complex_double check_complex(lua_State* L, int idx, void* p, CType* ct)
{
	if (ct->type == INVALID_TYPE) {
		double d = luaL_checknumber(L, idx);
#ifdef HAVE_COMPLEX
		return d;
#else
		complex_double c;
		c.real = d;
		c.imag = 0;
		return c;
#endif
	} else if (ct->type == COMPLEX_DOUBLE_TYPE) {
		return *(complex_double*) p;
	} else if (ct->type == COMPLEX_FLOAT_TYPE) {
		complex_float* f = (complex_float*) p;
#ifdef HAVE_COMPLEX
		return *f;
#else
		complex_double d;
		d.real = f->real;
		d.imag = f->imag;
		return d;
#endif
	} else {
		complex_double dummy;
		type_error(L, idx, "complex", 0, NULL);
		memset(&dummy, 0, sizeof(dummy));
		return dummy;
	}
}

static int rank(const CType* ct)
{
	if (ct->pointers) {
		return 5;
	}

	switch (ct->type) {
	case COMPLEX_DOUBLE_TYPE:
		return 7;
	case COMPLEX_FLOAT_TYPE:
		return 6;
	case INTPTR_TYPE:
		return sizeof(intptr_t) >= sizeof(int64_t) ? 4 : 1;
	case INT64_TYPE:
		return ct->is_unsigned ? 3 : 2;
	case INT32_TYPE:
	case INT16_TYPE:
	case INT8_TYPE:
		return 2;
	default:
		return 0;
	}
}

static void push_complex(lua_State* L, complex_double res, int ct_usr, const CType* ct)
{
	if (ct->type == COMPLEX_DOUBLE_TYPE) {
		complex_double* p = (complex_double*) push_cdata(L, ct_usr, ct);
		*p = res;
	} else {
		complex_float* p = (complex_float*) push_cdata(L, ct_usr, ct);
#ifdef HAVE_COMPLEX
		*p = (complex float) res;
#else
		p->real = (float) res.real;
		p->imag = (float) res.imag;
#endif
	}
}

static void push_number(lua_State* L, int64_t val, int ct_usr, const CType* ct)
{
	if ((ct->pointers || ct->type == INTPTR_TYPE) && sizeof(intptr_t) != sizeof(int64_t)) {
		intptr_t* p = (intptr_t*) push_cdata(L, ct_usr, ct);
		*p = val;
	} else {
		int64_t* p = (int64_t*) push_cdata(L, ct_usr, ct);
		*p = val;
	}
}

static int call_user_op(lua_State* L, const char* opfield, int idx, int ct_usr, const CType* ct)
{
	idx = lua_absindex(L, idx);

	if (!ct->pointers && push_user_mt(L, ct_usr, ct)) {
		lua_pushstring(L, opfield);
		lua_rawget(L, -2);
		if (!lua_isnil(L, -1)) {
			int top = lua_gettop(L);
			lua_pushvalue(L, idx);
			lua_call(L, 1, LUA_MULTRET);
			return lua_gettop(L) - top + 1;
		}
	  lua_pop(L, 2);
	}
	return -1;
}

static int cdata_unm(lua_State* L)
{
	CType ct;
	void* p;
	int64_t val;
	int ret;

	lua_settop(L, 1);
	p = to_cdata(L, 1, &ct);

	ret = call_user_op(L, "__unm", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	val = check_intptr(L, 1, p, &ct);

	if (ct.pointers) {
		luaL_error(L, "can't negate a pointer value");
	} else {
		memset(&ct, 0, sizeof(ct));
		ct.type = INT64_TYPE;
		ct.base_size = 8;
		ct.is_defined = 1;
		push_number(L, -val, 0, &ct);
	}

	return 1;
}

static int cdata_bnot(lua_State* L)
{
	CType ct;
	void* p;
	int64_t val;
	int ret;

	lua_settop(L, 1);
	p = to_cdata(L, 1, &ct);

	ret = call_user_op(L, "__bnot", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	val = check_intptr(L, 1, p, &ct);

	if (ct.pointers) {
		luaL_error(L, "can't bitwise-not a pointer value");
	} else {
		memset(&ct, 0, sizeof(ct));
		ct.type = INT64_TYPE;
		ct.base_size = 8;
		ct.is_defined = 1;
		push_number(L, ~val, 0, &ct);
	}

	return 1;
}


/* returns -ve if no binop was called otherwise returns the number of return
 * arguments */
static int call_user_binop(lua_State* L, const char* opfield, int lidx, int lusr, const CType* lt, int ridx, int rusr, const CType* rt)
{
	lidx = lua_absindex(L, lidx);
	ridx = lua_absindex(L, ridx);

	if (!lt->pointers && push_user_mt(L, lusr, lt)) {
		lua_pushstring(L, opfield);
		lua_rawget(L, -2);

		if (!lua_isnil(L, -1)) {
			int top = lua_gettop(L);
			lua_pushvalue(L, lidx);
			lua_pushvalue(L, ridx);
			lua_call(L, 2, LUA_MULTRET);
			return lua_gettop(L) - top + 1;
		}

		lua_pop(L, 2); /* user_mt and user_mt.op */
	}

	if (!rt->pointers && push_user_mt(L, rusr, rt)) {
		lua_pushstring(L, opfield);
		lua_rawget(L, -2);

		if (!lua_isnil(L, -1)) {
			int top = lua_gettop(L);
			lua_pushvalue(L, lidx);
			lua_pushvalue(L, ridx);
			lua_call(L, 2, LUA_MULTRET);
			return lua_gettop(L) - top + 1;
		}

		lua_pop(L, 2); /* user_mt and user_mt.op */
	}

	return -1;
}

static int cdata_concat(lua_State* L)
{
	CType lt, rt;
	int ret;

	lua_settop(L, 2);
	to_cdata(L, 1, &lt);
	to_cdata(L, 2, &rt);

	ret = call_user_binop(L, "__concat", 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
		return ret;
	}

	return luaL_error(L, "NYI");
}

static int cdata_len(lua_State* L)
{
	CType ct;
	int ret;

	lua_settop(L, 1);
	to_cdata(L, 1, &ct);

	ret = call_user_op(L, "__len", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	push_type_name(L, 2, &ct);
	return luaL_error(L, "type %s does not implement the __len metamethod", lua_tostring(L, -1));
}

static int cdata_pairs(lua_State* L)
{
	CType ct;
	int ret;

	lua_settop(L, 1);
	to_cdata(L, 1, &ct);

	ret = call_user_op(L, "__pairs", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	push_type_name(L, 2, &ct);
	return luaL_error(L, "type %s does not implement the __pairs metamethod", lua_tostring(L, -1));
}

static int cdata_ipairs(lua_State* L)
{
	CType ct;
	int ret;

	lua_settop(L, 1);
	to_cdata(L, 1, &ct);

	ret = call_user_op(L, "__ipairs", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	push_type_name(L, 2, &ct);
	return luaL_error(L, "type %s does not implement the __ipairs metamethod", lua_tostring(L, -1));
}

static int cdata_add(lua_State* L)
{
	CType lt, rt, ct;
	void *lp, *rp;
	int ct_usr;
	int ret;

	lua_settop(L, 2);

	lp = to_cdata(L, 1, &lt);
	rp = to_cdata(L, 2, &rt);
	assert(lua_gettop(L) == 4);

	ret = call_user_binop(L, "__add", 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
		return ret;
	}
	assert(lua_gettop(L) == 4);

	ct_usr = rank(&lt) > rank(&rt) ? 3 : 4;
	ct = rank(&lt) > rank(&rt) ? lt : rt;

	if (IS_COMPLEX(ct.type)) {
		complex_double left, right, res;

		left = check_complex(L, 1, lp, &lt);
		right = check_complex(L, 2, rp, &rt);
		assert(lua_gettop(L) == 4);

#ifdef HAVE_COMPLEX
		res = left + right;
#else
		res.real = left.real + right.real;
		res.imag = left.imag + right.imag;
#endif

		push_complex(L, res, ct_usr, &ct);
		return 1;

	} else {
		int64_t left = check_intptr(L, 1, lp, &lt);
		int64_t right = check_intptr(L, 2, rp, &rt);
		assert(lua_gettop(L) == 4);

		/* note due to 2s complement it doesn't matter if we do the addition as int or uint,
		 * but the result needs to be uint64_t if either of the sources are */

		if (lt.pointers && rt.pointers) {
			luaL_error(L, "can't add two pointers");

		} else if (lt.pointers) {
			int64_t res = left + (lt.pointers > 1 ? sizeof(void*) : lt.base_size) * right;
			lt.is_array = 0;
			push_number(L, res, 3, &lt);

		} else if (rt.pointers) {
			int64_t res = right + (rt.pointers > 1 ? sizeof(void*) : rt.base_size) * left;
			rt.is_array = 0;
			push_number(L, res, 4, &rt);

		} else {
			push_number(L, left + right, ct_usr, &ct);
		}

		return 1;
	}
}

static int cdata_sub(lua_State* L)
{
	CType lt, rt, ct;
	void *lp, *rp;
	int ct_usr;
	int ret;

	lua_settop(L, 2);

	lp = to_cdata(L, 1, &lt);
	rp = to_cdata(L, 2, &rt);

	ret = call_user_binop(L, "__sub", 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
		return ret;
	}

	ct_usr = rank(&lt) > rank(&rt) ? 3 : 4;
	ct = rank(&lt) > rank(&rt) ? lt : rt;

	if (IS_COMPLEX(ct.type)) {
		complex_double left, right, res;

		left = check_complex(L, 1, lp, &lt);
		right = check_complex(L, 2, rp, &rt);

#ifdef HAVE_COMPLEX
		res = left - right;
#else
		res.real = left.real - right.real;
		res.imag = left.imag - right.imag;
#endif

		push_complex(L, res, ct_usr, &ct);
		return 1;

	} else {
		int64_t left = check_intptr(L, 1, lp, &lt);
		int64_t right = check_intptr(L, 2, rp, &rt);

		if (rt.pointers && lt.pointers) {
			int64_t res = (left - right) / (lt.pointers > 1 ? sizeof(void*) : lt.base_size);
			//push_number(L, res, 0, &ct);
			lua_pushinteger(L, res);		//push int or int64?

		} else if (lt.pointers) {
			int64_t res = left - (lt.pointers > 1 ? sizeof(void*) : lt.base_size) * right;
			lt.is_array = 0;
			push_number(L, res, 3, &lt);

		} else {
			int64_t res = left - right;
			push_number(L, res, ct_usr, &ct);
		}

		return 1;
	}
}

// TODO fix for unsigned
#define NUMBER_ONLY_BINOP(OPSTR, DO_NORMAL, DO_COMPLEX)	                 \
	CType lt, rt, ct;                                                \
	void *lp, *rp;                                                          \
	int ct_usr;                                                             \
	int ret;                                                                \
	                                                                        \
	lua_settop(L, 2);                                                       \
	                                                                        \
	lp = to_cdata(L, 1, &lt);                                               \
	rp = to_cdata(L, 2, &rt);                                               \
	                                                                        \
	ret = call_user_binop(L, OPSTR, 1, 3, &lt, 2, 4, &rt);                  \
	if (ret >= 0) {                                                         \
	    return ret;                                                         \
	}                                                                       \
	                                                                        \
	ct_usr = rank(&lt) > rank(&rt) ? 3 : 4;                                 \
	ct = rank(&lt) > rank(&rt) ? lt : rt;                                   \
	                                                                        \
	if (IS_COMPLEX(ct.type)) {                                              \
	    complex_double res;                                                 \
	    complex_double left = check_complex(L, 1, lp, &lt);                 \
	    complex_double right = check_complex(L, 2, rp, &rt);                \
	                                                                        \
	    DO_COMPLEX(left, right, res);                                       \
	    push_complex(L, res, ct_usr, &ct);                                  \
	                                                                        \
	} else if (lt.pointers || rt.pointers) {                                \
	    luaL_error(L, "can't operate on a pointer value");                  \
	                                                                        \
	} else {                                                                \
	    int64_t res;                                                        \
	    int64_t left = check_intptr(L, 1, lp, &lt);                         \
	    int64_t right = check_intptr(L, 2, rp, &rt);                        \
	                                                                        \
	    DO_NORMAL(left, right, res);                                        \
	    push_number(L, res, ct_usr, &ct);                                   \
	}                                                                       \
	                                                                        \
	return 1

#define MUL(l,r,s) s = l * r
#define DIV(l,r,s) s = l / r
#define MOD(l,r,s) s = l % r
#define POW(l,r,s) s = pow(l, r)
#define IDIV(l,r,s) s = (lua_Integer)(l / r)	//TODO copy out of lua-5.4.7/src/lvm.c
#define BAND(l,r,s) s = ((lua_Integer)l & (lua_Integer)r)
#define BOR(l,r,s) s = ((lua_Integer)l | (lua_Integer)r)
#define BXOR(l,r,s) s = ((lua_Integer)l ^ (lua_Integer)r)
#define SHL(l,r,s) s = ((lua_Integer)l << (lua_Integer)r)
#define SHR(l,r,s) s = ((lua_Integer)l >> (lua_Integer)r)

#ifdef HAVE_COMPLEX
#define MULC(l,r,s) s = l * r
#define DIVC(l,r,s) s = l / r
#define MODC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex mod")
#define POWC(l,r,s) s = cpow(l, r)
#else
#define MULC(l,r,s) s.real = l.real * r.real - l.imag * r.imag, s.imag = l.real * r.imag + l.imag * r.real
#define DIVC(l,r,s) s.real = (l.real * r.real + l.imag * r.imag) / (r.real * r.real + r.imag * r.imag), \
					s.imag = (l.imag * r.real - l.real * r.imag) / (r.real * r.real + r.imag * r.imag)
#define MODC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex mod")
#define POWC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex pow")
#endif
#define IDIVC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex idiv")
#define BANDC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex band")
#define BORC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex bor")
#define BXORC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex bxor")
#define SHLC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex shl")
#define SHRC(l,r,s) (void) l, (void) r, memset(&s, 0, sizeof(s)), luaL_error(L, "NYI: complex shr")

static int cdata_mul(lua_State* L) { NUMBER_ONLY_BINOP("__mul", MUL, MULC); }
static int cdata_div(lua_State* L) { NUMBER_ONLY_BINOP("__div", DIV, DIVC); }
static int cdata_mod(lua_State* L) { NUMBER_ONLY_BINOP("__mod", MOD, MODC); }
static int cdata_pow(lua_State* L) { NUMBER_ONLY_BINOP("__pow", POW, POWC); }
static int cdata_idiv(lua_State* L) { NUMBER_ONLY_BINOP("__idiv", IDIV, IDIVC); }
static int cdata_band(lua_State* L) { NUMBER_ONLY_BINOP("__band", BAND, BANDC); }
static int cdata_bor(lua_State* L) { NUMBER_ONLY_BINOP("__bor", BOR, BORC); }
static int cdata_bxor(lua_State* L) { NUMBER_ONLY_BINOP("__bxor", BXOR, BXORC); }
static int cdata_shl(lua_State* L) { NUMBER_ONLY_BINOP("__shl", SHL, SHLC); }
static int cdata_shr(lua_State* L) { NUMBER_ONLY_BINOP("__shr", SHR, SHRC); }

#define EQ(l, r) (l) == (r)
#define LT(l, r) (l) < (r)
#define LE(l, r) (l) <= (r)

#ifdef HAVE_COMPLEX
#define EQC(l, r) (l) == (r)
#else
#define EQC(l, r) (l).real == (r).real && (l).imag == (r).imag
#endif

#define LEC(l, r) EQC(l, r), luaL_error(L, "complex numbers are non-orderable")
#define LTC(l, r) EQC(l, r), luaL_error(L, "complex numbers are non-orderable")

static int cdata_eq(lua_State* L)
{
	char const * opstr = "__eq";
#define OP EQ
#define OPC EQC

	CType lt, rt;
	void *lp, *rp;
	int ret, res;

	lua_settop(L, 2);

	lp = to_cdata(L, 1, &lt);
	rp = to_cdata(L, 2, &rt);

	ret = call_user_binop(L, opstr, 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
	    return ret;
	}

	if (IS_COMPLEX(lt.type) || IS_COMPLEX(rt.type)) {
	    complex_double left = check_complex(L, 1, lp, &lt);
	    complex_double right = check_complex(L, 2, rp, &rt);

	    res = OPC(left, right);

	    lua_pushboolean(L, res);

	} else {
	    int64_t left = check_intptr(L, 1, lp, &lt);
	    int64_t right = check_intptr(L, 2, rp, &rt);

		// Chris: what a mess.  putting this here for now. Don't care about false-positives
		if (lt.type == FUNCTION_TYPE
			|| lt.type == FUNCTION_PTR_TYPE
			|| rt.type == FUNCTION_TYPE
			|| rt.type == FUNCTION_PTR_TYPE
		) {
			lua_pushboolean(L, OP((uint64_t) left, (uint64_t) right));
			return 1;
		}

	    if (lt.pointers && rt.pointers) {
	        if (is_void_ptr(&lt) || is_void_ptr(&rt) || is_same_type(L, 3, 4, &lt, &rt)) {
	            res = OP((uint64_t) left, (uint64_t) right);
	        } else {
	            goto err;
	        }

	    } else if (lt.is_null && (rt.type == FUNCTION_PTR_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.is_null && (lt.type == FUNCTION_PTR_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.pointers && rt.type == INTPTR_TYPE && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers && lt.type == INTPTR_TYPE && lt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers || lt.pointers) {
	        goto err;

	    } else if (lt.is_unsigned && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.is_unsigned) {
	        res = OP((int64_t) (uint64_t) left, right);

	    } else if (rt.is_unsigned) {
	        res = OP(left, (int64_t) (uint64_t) right);

	    } else {
	        res = OP(left, right);
	    }

	    lua_pushboolean(L, res);
	}
	return 1;

#undef OP
#undef OPC

err:
	lua_pushboolean(L, 0);
	return 1;
}

static int cdata_lt(lua_State* L)
{
	char const * opstr = "__lt";
#define OP LT
#define OPC LTC

	CType lt, rt;
	void *lp, *rp;
	int ret, res;

	lua_settop(L, 2);

	lp = to_cdata(L, 1, &lt);
	rp = to_cdata(L, 2, &rt);

	ret = call_user_binop(L, opstr, 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
	    return ret;
	}

	if (IS_COMPLEX(lt.type) || IS_COMPLEX(rt.type)) {
	    complex_double left = check_complex(L, 1, lp, &lt);
	    complex_double right = check_complex(L, 2, rp, &rt);

	    res = OPC(left, right);

	    lua_pushboolean(L, res);

	} else {
	    int64_t left = check_intptr(L, 1, lp, &lt);
	    int64_t right = check_intptr(L, 2, rp, &rt);

		// Chris: what a mess.  putting this here for now. Don't care about false-positives
		if (lt.type == FUNCTION_TYPE
			|| lt.type == FUNCTION_PTR_TYPE
			|| rt.type == FUNCTION_TYPE
			|| rt.type == FUNCTION_PTR_TYPE
		) {
			lua_pushboolean(L, OP((uint64_t) left, (uint64_t) right));
			return 1;
		}

	    if (lt.pointers && rt.pointers) {
	        if (is_void_ptr(&lt) || is_void_ptr(&rt) || is_same_type(L, 3, 4, &lt, &rt)) {
	            res = OP((uint64_t) left, (uint64_t) right);
	        } else {
	            goto err;
	        }

	    } else if (lt.is_null && (rt.type == FUNCTION_PTR_TYPE || rt.type == FUNCTION_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.is_null && (lt.type == FUNCTION_PTR_TYPE || lt.type == FUNCTION_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.pointers && rt.type == INTPTR_TYPE && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers && lt.type == INTPTR_TYPE && lt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers || lt.pointers) {
	        goto err;

	    } else if (lt.is_unsigned && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.is_unsigned) {
	        res = OP((int64_t) (uint64_t) left, right);

	    } else if (rt.is_unsigned) {
	        res = OP(left, (int64_t) (uint64_t) right);

	    } else {
	        res = OP(left, right);
	    }

	    lua_pushboolean(L, res);
	}
	return 1;

#undef OP
#undef OPC

err:
	lua_getuservalue(L, 1);
	lua_getuservalue(L, 2);
	push_type_name(L, -2, &lt);
	push_type_name(L, -2, &lt);
	return luaL_error(L, "trying to compare incompatible types %s and %s", lua_tostring(L, -2), lua_tostring(L, -1));
}

static int cdata_le(lua_State* L)
{

	char const * opstr = "__le";
#define OP LE
#define OPC LEC

	CType lt, rt;
	void *lp, *rp;
	int ret, res;

	lua_settop(L, 2);

	lp = to_cdata(L, 1, &lt);
	rp = to_cdata(L, 2, &rt);

	ret = call_user_binop(L, opstr, 1, 3, &lt, 2, 4, &rt);
	if (ret >= 0) {
	    return ret;
	}

	if (IS_COMPLEX(lt.type) || IS_COMPLEX(rt.type)) {
	    complex_double left = check_complex(L, 1, lp, &lt);
	    complex_double right = check_complex(L, 2, rp, &rt);

	    res = OPC(left, right);

	    lua_pushboolean(L, res);

	} else {
	    int64_t left = check_intptr(L, 1, lp, &lt);
	    int64_t right = check_intptr(L, 2, rp, &rt);

		// Chris: what a mess.  putting this here for now. Don't care about false-positives
		if (lt.type == FUNCTION_TYPE
			|| lt.type == FUNCTION_PTR_TYPE
			|| rt.type == FUNCTION_TYPE
			|| rt.type == FUNCTION_PTR_TYPE
		) {
			lua_pushboolean(L, OP((uint64_t) left, (uint64_t) right));
			return 1;
		}

	    if (lt.pointers && rt.pointers) {
	        if (is_void_ptr(&lt) || is_void_ptr(&rt) || is_same_type(L, 3, 4, &lt, &rt)) {
	            res = OP((uint64_t) left, (uint64_t) right);
	        } else {
	            goto err;
	        }

	    } else if (lt.is_null && (rt.type == FUNCTION_PTR_TYPE || rt.type == FUNCTION_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.is_null && (lt.type == FUNCTION_PTR_TYPE || lt.type == FUNCTION_TYPE)) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.pointers && rt.type == INTPTR_TYPE && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers && lt.type == INTPTR_TYPE && lt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (rt.pointers || lt.pointers) {
	        goto err;

	    } else if (lt.is_unsigned && rt.is_unsigned) {
	        res = OP((uint64_t) left, (uint64_t) right);

	    } else if (lt.is_unsigned) {
	        res = OP((int64_t) (uint64_t) left, right);

	    } else if (rt.is_unsigned) {
	        res = OP(left, (int64_t) (uint64_t) right);

	    } else {
	        res = OP(left, right);
	    }

	    lua_pushboolean(L, res);
	}
	return 1;

#undef OP
#undef OPC

err:
	lua_getuservalue(L, 1);
	lua_getuservalue(L, 2);
	push_type_name(L, -2, &lt);
	push_type_name(L, -2, &lt);
	return luaL_error(L, "trying to compare incompatible types %s and %s", lua_tostring(L, -2), lua_tostring(L, -1));
}

static const char* etype_tostring(int type) {
	switch (type) {
	case VOID_TYPE: return "void";
	case DOUBLE_TYPE: return "double";
	case FLOAT_TYPE: return "float";
	case COMPLEX_DOUBLE_TYPE: return "complex double";
	case COMPLEX_FLOAT_TYPE: return "complex float";
	case BOOL_TYPE: return "bool";
	case INT8_TYPE: return "int8";
	case INT16_TYPE: return "int16";
	case INT32_TYPE: return "int32";
	case INT64_TYPE: return "int64";
	case INTPTR_TYPE: return "intptr";
	case ENUM_TYPE: return "enum";
	case UNION_TYPE: return "union";
	case STRUCT_TYPE: return "struct";
	case FUNCTION_PTR_TYPE: return "function ptr";
	case FUNCTION_TYPE: return "function";
	default: return "invalid";
	}
}

void print_type(lua_State* L, const CType* ct)
{
	lua_pushfstring(L, " sz %d %d %d align %d ptr %d %d %d type %s%s %d %d %d name %d call %d %d var %d %d %d bit %d %d %d %d jit %d",
			/* sz */
			ct->base_size,
			ct->array_size,
			ct->offset,
			/* align */
			ct->align_mask,
			/* ptr */
			ct->is_array,
			ct->pointers,
			ct->const_mask,
			/* type */
			ct->is_unsigned ? "u" : "",
			etype_tostring(ct->type),
			ct->is_reference,
			ct->is_defined,
			ct->is_null,
			/* name */
			ct->has_member_name,
			/* call */
			ct->calling_convention,
			ct->has_var_arg,
			/* var */
			ct->is_variable_array,
			ct->is_variable_struct,
			ct->variable_size_known,
			/* bit */
			ct->is_bitfield,
			ct->has_bitfield,
			ct->bit_offset,
			ct->bit_size,
			/* jit */
			ct->is_jitted);
}

static int ctype_tostring(lua_State* L)
{
	CType ct;
	assert(lua_type(L, 1) == LUA_TUSERDATA);
	lua_settop(L, 1);
	check_ctype(L, 1, &ct, 0);
	assert(lua_gettop(L) == 2);
	push_type_name(L, -1, &ct);
	lua_pushfstring(L, "ctype<%s>", lua_tostring(L, -1));

	if (DEBUG_TOSTRING) {
		print_type(L, &ct);
		lua_concat(L, 2);
	}

	return 1;
}

// ctype's __index points back to its metatype's __index
static int ctype_index(lua_State * L) {
	CType ct;
	assert(lua_type(L, 1) == LUA_TUSERDATA);
	check_ctype(L, 1, &ct, 0);

	// taken from cdata_index

	// if we're indexing into a ctype object
	// and it's an arrya-of-something
	// then we don't want it interacting with the base type's metatable
	if (ct.is_array) {
		goto err;
	}

	assert(lua_gettop(L) == 3);
	if (!push_user_mt(L, -1, &ct)) {
		goto err;
	}

	lua_pushliteral(L, "__index");
	lua_rawget(L, -2);

	if (lua_isnil(L, -1)) {
		goto err;
	}

	if (lua_istable(L, -1)) {
		lua_pushvalue(L, 2);
		lua_gettable(L, -2);
		return 1;
	}

	lua_insert(L, 1);
	lua_settop(L, 3);
	lua_call(L, 2, LUA_MULTRET);
	return lua_gettop(L);

err:
	push_type_name(L, 3, &ct);
	return luaL_error(L, "type %s has no member %s", lua_tostring(L, -1), lua_tostring(L, 2));
}

static int ctype_eq(lua_State * L) {
	CType a, b;
	check_ctype(L, 1, &a, 0);
	check_ctype(L, 2, &b, 0);
	lua_pushboolean(L, !memcmp(&a, &b, sizeof(a)));
	return 1;
}

static int cdata_tostring(lua_State * L) {
	char buf[64];

	lua_settop(L, 1);
	CType ct;
	void * p = to_cdata(L, 1, &ct);

	int ret = call_user_op(L, "__tostring", 1, 2, &ct);
	if (ret >= 0) {
		return ret;
	}

	if (ct.pointers > 0 || ct.is_reference || ct.type == STRUCT_TYPE || ct.type == UNION_TYPE) {
		push_type_name(L, -1, &ct);
		lua_pushfstring(L, "cdata<%s>: %p", lua_tostring(L, -1), p);

		if (DEBUG_TOSTRING) {
			print_type(L, &ct);
			lua_concat(L, 2);
		}

		return 1;
	}

	switch (ct.type) {
	case COMPLEX_DOUBLE_TYPE:
		{
			complex_double c = *(complex_double*) p;
			lua_pushfstring(L, "%f+%fi", creal(c), cimag(c));
		}
		return 1;

	case COMPLEX_FLOAT_TYPE:
		{
			complex_float c = *(complex_float*) p;
			lua_pushfstring(L, "%f+%fi", crealf(c), cimagf(c));
		}
		return 1;

	case FUNCTION_PTR_TYPE:
		push_type_name(L, -1, &ct);
//		p = *(void**) p;
		lua_pushfstring(L, "cdata<%s>: %p", lua_tostring(L, -1), *(void**) p);
		return 1;

	// Chris:
	// if a FUNCTION_PTR_TYPE is a void(*)() returned from a C function
	// then is a FUNCTION_TYPE the C function itself?
	// I stil don't get the difference... except to distinguish extra resources each uses behind the scenes ...
	case FUNCTION_TYPE:
		// Chris: here for my CALL_WITH_LIBFFI cdata wrapping C functions
		push_type_name(L, -1, &ct);
		lua_pushfstring(L, "cdata<%s>: %p", lua_tostring(L, -1), *(void**)p);
		return 1;

	case INTPTR_TYPE:
		lua_pushfstring(L, "%p", *(uintptr_t*) p);
		return 1;

	case INT64_TYPE:
		sprintf(buf, ct.is_unsigned ? "%" PRIu64 : "%" PRId64, *(uint64_t*) p);
		lua_pushstring(L, buf);
		return 1;

	default:
		sprintf(buf, ct.is_unsigned ? "%" PRId64 : "%" PRId64, (int64_t) check_intptr(L, 1, p, &ct));
		lua_pushstring(L, buf);
		return 1;
	}
}

static int ffi_errno(lua_State* L)
{
	JIT* jit = get_jit(L);

	if (!lua_isnoneornil(L, 1)) {
		lua_pushinteger(L, jit->last_errno);
		jit->last_errno = luaL_checknumber(L, 1);
	} else {
		lua_pushinteger(L, jit->last_errno);
	}

	return 1;
}

static int ffi_type(lua_State* L) {		// stack: x, ...
	if (lua_isuserdata(L, 1) && lua_getmetatable(L, 1)) {
										// stack: x, ..., x's metatable
		if (equalsRegistry(L, -1, &cdata_mt_key) || equalsRegistry(L, -1, &ctype_mt_key)) {
			lua_pushstring(L, "cdata");	// stack: x, ..., x's metatable, "cdata"
			return 1;
		}
		lua_pop(L, 1); 					// stack: x, ...
	}

	// call the old _G.type, we use an upvalue as _G.type is set to this function
	lua_pushvalue(L, lua_upvalueindex(1));		// stack: x, ..., old type
	lua_insert(L, 1);							// stack: type, x, ...
	lua_call(L, lua_gettop(L)-1, LUA_MULTRET);	// type(x, ...)'s results...
	return lua_gettop(L);
}

static int ffi_number(lua_State* L) {
														// stack: x, ...
	CType ct;
	void * data = to_cdata(L, 1, &ct);					// stack: x, ..., x's uservalue[1] or nil

	// not cdata <=> handle default case
	if (ct.type == INVALID_TYPE) {
		// call the old _G.tonumber, we use an upvalue as _G.tonumber is set to this function
		lua_pop(L, 1);									// stack: x, ...
		lua_pushvalue(L, lua_upvalueindex(1));			// stack: x, ..., old tonumber
		lua_insert(L, 1);								// stack: tonumber, x, ...
		lua_call(L, lua_gettop(L)-1, LUA_MULTRET);		// stack: tonumber(x, ...) results...
		return lua_gettop(L);
	}

	// How do you tell if something is just not a primitive?
	// I bet I'll catch too many types in here, like refs to primitive fields -- watch out for that.
	if (ct.type == VOID_TYPE
		|| ct.type == UNION_TYPE
		|| ct.type == STRUCT_TYPE
		|| ct.type == FUNCTION_TYPE
		|| ct.type == FUNCTION_PTR_TYPE
		|| ct.is_array
		|| ct.is_variable_array				// I think this is only set if is_array is set ... if so this test can be removed.
		|| ct.pointers
	) {
		lua_pushnil(L);									// stack: x, ..., x's uservalue[1], nil
		return 1;
	}

	if (ct.type == FLOAT_TYPE || ct.type == COMPLEX_FLOAT_TYPE) {
		lua_pushnumber(L, *(float*)data);				// stack: x, ..., x's uservalue[1], *(float*)data
		return 1;
	}

	if (ct.type == DOUBLE_TYPE || ct.type == COMPLEX_DOUBLE_TYPE) {
		lua_pushnumber(L, *(double*)data);				// stack: x, ..., x's uservalue[1], *(double*)data
		return 1;
	}

	// assume it's an integer type
	lua_pushinteger(L, check_intptr(L, 1, data, &ct));
	return 1;
}

static int ffi_string(
	lua_State * L
) {
	CType ct;
	char* data;
	lua_settop(L, 2);

	data = (char*) check_cdata(L, 1, &ct);

	if (is_void_ptr(&ct)) {
		lua_pushlstring(L, data, (size_t) luaL_checknumber(L, 2));
		return 1;

	} else if (
		//ct.type == INT8_TYPE && // luajit allows ffi.string of any kind of pointer when you define the size
		ct.pointers == 1
	) {
		size_t sz;

		if (lua_isuserdata(L, 2)) {
			ptrdiff_t val;
			if (!cdata_tointeger(L, 2, &val)) {
				type_error(L, 2, "int", 0, NULL);
			}
			sz = (size_t) val;
		} else if (!lua_isnil(L, 2)) {
			sz = (size_t) luaL_checknumber(L, 2);

		} else if (ct.is_array && !ct.is_variable_array) {
			// array type without definite size?  error if not char*...
			if (ct.type != INT8_TYPE) return luaL_error(L, "cannot convert cdata to string");

			char* nul = (char*)memchr(data, '\0', ct.array_size);
			sz = nul ? nul - data : ct.array_size;

		} else {
			// array type without definite size?  error if not char*...
			if (ct.type != INT8_TYPE) return luaL_error(L, "cannot convert cdata to string");

			sz = strlen(data);
		}

		lua_pushlstring(L, data, sz);
		return 1;
	}

	return luaL_error(L, "cannot convert cdata to string");
}

static int ffi_copy(lua_State* L)
{
	CType ft, tt;
	char *to, *from;

	setmintop(L, 3);
	to = (char*) check_pointer(L, 1, &tt);
	from = (char*) check_pointer(L, 2, &ft);

	if (!lua_isnoneornil(L, 3)) {
		memcpy(to, from, (size_t) luaL_checknumber(L, 3));

	} else if (
		//ft.type == INT8_TYPE && // luajit allows ffi.string of any kind of pointer
		ft.pointers == 1
	) {
		size_t sz = ft.is_array ? (ft.base_size * ft.array_size) : strlen(from);
		memcpy(to, from, sz);
		to[sz] = '\0';
	}

	return 0;
}

static int ffi_fill(lua_State* L)
{
	CType ct;
	void* to;
	size_t sz;
	int val = 0;

	setmintop(L, 3);
	to = check_pointer(L, 1, &ct);
	sz = (size_t) luaL_checknumber(L, 2);

	if (!lua_isnoneornil(L, 3)) {
		val = (int) luaL_checkinteger(L, 3);
	}

	memset(to, val, sz);
	return 0;
}

/*
ffi.abi(key) returns a boolean cast value of whatever is in the registry[&abi_key].
Why not use a proper C struct instead of a Lua table?
*/
static int ffi_abi(lua_State * L) {
	luaL_checkstring(L, 1);
	pushRegistry(L, &abi_key);
	lua_pushvalue(L, 1);
	lua_rawget(L, -2);
	lua_pushboolean(L, lua_toboolean(L, -1));
	return 1;
}

static int ffi_load(lua_State* L)
{
	const char* libname = luaL_checkstring(L, 1);
	void** lib = (void**) lua_newuserdata(L, sizeof(void*));

	*lib = LoadLibraryA(libname);

#ifdef LIB_FORMAT_1
	if (!*lib) {
		libname = lua_pushfstring(L, LIB_FORMAT_1, lua_tostring(L, 1));
		*lib = LoadLibraryA(libname);
		lua_pop(L, 1);
	}
#endif

#ifdef LIB_FORMAT_2
	if (!*lib) {
		libname = lua_pushfstring(L, LIB_FORMAT_2, lua_tostring(L, 1));
		*lib = LoadLibraryA(libname);
		lua_pop(L, 1);
	}
#endif

	if (!*lib) {
		return luaL_error(L, "could not load library %s", lua_tostring(L, 1));
	}

	lua_newtable(L);
	lua_setuservalue(L, -2);

	pushRegistry(L, &cmodule_mt_key);
	lua_setmetatable(L, -2);
	return 1;
}

static void* find_symbol(lua_State* L, int modidx, const char* asmname)
{
	size_t i;
	void** libs;
	size_t num;
	void* sym = NULL;

	libs = (void**) lua_touserdata(L, modidx);
	num = lua_rawlen(L, modidx) / sizeof(void*);

	for (i = 0; i < num && sym == NULL; i++) {
		if (libs[i]) {
			sym = GetProcAddressA(libs[i], asmname);
		}
	}

	return sym;
}

/*
pushes the user table
*/
static void * lookup_global(
	lua_State * L,
	int modidx,
	int nameidx,
	const char ** pname,	// out
	CType * ct				// out
) {											// stack: ...
	int top = lua_gettop(L);

	modidx = lua_absindex(L, modidx);
	nameidx = lua_absindex(L, nameidx);

	*pname = luaL_checkstring(L, nameidx);

	// get the ctype
	pushRegistry(L, &functions_key);		// stack: ..., registry[&functions_key]
	lua_pushvalue(L, nameidx);				// stack: ..., registry[&functions_key], name = stack[nameidx]
	lua_rawget(L, -2);						// stack: ..., registry[&functions_key], ct = registry[&functions_key][name]
	if (lua_isnil(L, -1)) {
		luaL_error(L, "missing declaration for function/global %s", *pname);
		return NULL;
	}

	// leave just the ct_usr on the stack
	*ct = *(const CType*)lua_touserdata(L, -1);
	lua_getuservalue(L, -1);			// stack: ..., registry[&functions_key], ct, uv = ct uservalue[1]
	lua_replace(L, top + 1);			// stack: ..., uv, ct
	lua_pop(L, 1);						// stack: ..., uv

	assert(lua_gettop(L) == top + 1);

	// get the assembly name
	pushRegistry(L, &asmname_key);		// stack: ..., uv, registry[&asmname_key]
	lua_pushvalue(L, nameidx);			// stack: ..., uv, registry[&asmname_key], name
	lua_rawget(L, -2);					// stack: ..., uv, registry[&asmname_key], registry[&asmname_key][name]
	if (lua_isstring(L, -1)) {
		*pname = lua_tostring(L, -1);
	}
	lua_pop(L, 2);						// stack: ..., uv

	void * sym = find_symbol(L, modidx, *pname);

	assert(lua_gettop(L) == top + 1);	// stack: ..., uv
	return sym;
}

/*
indexing a module (i.e. ffi.C, ffi.load(libname), etc)
checks in:
- module uservalue[1][key]
- registry[&constants_key][key]
*/
static int cmodule_index(
	lua_State* L
) {							// stack: module, key, ... but the ... is empty if it is invoked via metamethod
	lua_settop(L, 2);

	// see if we have already saved the function from our last time around in the compile_function() block down below
	lua_getuservalue(L, 1);	// stack: module, key, ..., module_uv = module uservalue[1]
	lua_pushvalue(L, 2);	// stack: module, key, ..., module_uv, key
	lua_rawget(L, -2);		// stack: module, key, ..., module_uv, module_uv[key]
	if (!lua_isnil(L, -1)) {
		// ... so module[key]'s function cdata is stored in module uservalue[1] [key] ?
		// why not just in ... module[key] ?
		return 1;
	}
	lua_pop(L, 2);			// stack: module, key, ...

	// check the constants table
	pushRegistry(L, &constants_key);	// stack: module, key, ..., registry[&constants_key]
	lua_pushvalue(L, 2);				// stack: module, key, ..., registry[&constants_key], key
	lua_rawget(L, -2);					// stack: module, key, ..., registry[&constants_key][key]
	if (!lua_isnil(L, -1)) {
		return 1;
	}
	lua_pop(L, 2);						// stack: module, key, ...

	// lookup_global pushes the ct_usr
	const char* asmname;
	CType ct;
	void * sym = lookup_global(L, 1, 2, &asmname, &ct);		// stack: module, key, ..., ct_usr = global[name]'s ctype's uservalue[1] ...

#if defined _WIN32 && !defined _WIN64 && (defined __i386__ || defined _M_IX86)
	if (!sym && ct.type == FUNCTION_TYPE) {
		ct.calling_convention = STD_CALL;
		lua_pushfstring(L, "_%s@%d", asmname, x86_return_size(L, -1, &ct));
		sym = find_symbol(L, 1, lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	if (!sym && ct.type == FUNCTION_TYPE) {
		ct.calling_convention = FAST_CALL;
		lua_pushfstring(L, "@%s@%d", asmname, x86_return_size(L, -1, &ct));
		sym = find_symbol(L, 1, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
#endif

	if (!sym) {
		return luaL_error(L, "failed to find function/global %s", asmname);
	}

	// NOTICE: This is going to error if anyone calls the module's __index manually from Lua
	assert(lua_gettop(L) == 3); 		// stack: module, key, ct_usr

	if (ct.type == FUNCTION_TYPE) {
		compile_function(L, (CFunction)sym, -1, &ct); 	// stack: module, key, ct_usr, closure_lua_CFunction
		assert(lua_gettop(L) == 4);

		// Set module_uv[key] = closure_lua_CFunction
		lua_getuservalue(L, 1);			// stack: module, key, ct_usr, closure_lua_CFunction, module_uv = module uservalue[1]
		lua_pushvalue(L, 2);			// stack: module, key, ct_usr, closure_lua_CFunction, module_uv, key
		lua_pushvalue(L, -3);			// stack: module, key, ct_usr, closure_lua_CFunction, module_uv, key, closure_lua_CFunction
		lua_rawset(L, -3);				// stack: module, key, ct_usr, closure_lua_CFunction, module_uv;  module_uv[key] = closure_lua_CFunction
		lua_pop(L, 1); 					// stack: module, key, ct_usr, closure_lua_CFunction
		return 1;
	}

	// extern const char* foo; and extern const char foo[];
	if (ct.pointers == 1 && ct.type == INT8_TYPE) {
		lua_pushstring(L, !ct.is_array
			? *(char**) sym
			: (char *)sym);
		return 1;
	}

	// extern struct foo foo[], extern void* foo[]; and extern struct foo foo;
	if (ct.is_array
		|| (!ct.pointers
			&& (ct.type == UNION_TYPE || ct.type == STRUCT_TYPE)
		)
	) {
		ct.is_reference = 1;
		void * p = push_cdata(L, -1, &ct);
		*(void**) p = sym;
		return 1;
	}

	// extern void* foo; and extern void (*foo)();
	if (ct.pointers || ct.type == FUNCTION_PTR_TYPE) {
		void* p = push_cdata(L, -1, &ct);
		*(void**) p = *(void**) sym;
		return 1;
	}

	switch (ct.type) {
	case COMPLEX_DOUBLE_TYPE:
	case COMPLEX_FLOAT_TYPE:
	case INTPTR_TYPE:
	case INT64_TYPE:
		{
			// TODO: complex float/double need to be references if .re and .imag are setable
			void* p = push_cdata(L, -1, &ct);
			memcpy(p, sym, ct.base_size);
			return 1;
		}

	case DOUBLE_TYPE:
		lua_pushnumber(L, *(double*) sym);
		return 1;

	case FLOAT_TYPE:
		lua_pushnumber(L, *(float*) sym);
		return 1;

	case BOOL_TYPE:
		lua_pushboolean(L, *(bool*) sym);
		return 1;

	case INT8_TYPE:
		lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint8_t*) sym : (lua_Integer) *(int8_t*) sym);
		return 1;

	case INT16_TYPE:
		lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint16_t*) sym : (lua_Integer) *(int16_t*) sym);
		return 1;

	case INT32_TYPE:
	case ENUM_TYPE:
		lua_pushinteger(L, ct.is_unsigned ? (lua_Integer) *(uint32_t*) sym : (lua_Integer) *(int32_t*) sym);
		return 1;
	}

	return luaL_error(L, "NYI - global value type");
}

static int cmodule_newindex(lua_State* L)
{
	const char* name;
	void* sym;
	CType ct;

	lua_settop(L, 3);

	/* pushes the ct_usr */
	sym = lookup_global(L, 1, 2, &name, &ct);
	assert(lua_gettop(L) == 4); /* module, name, value, ct_usr */

	if (sym == NULL) {
		return luaL_error(L, "failed to find global %s", name);
	}

	if (ct.type == FUNCTION_TYPE || ct.is_array || (ct.const_mask & 1)) {
		return luaL_error(L, "can not set global %s", name);
	}

	set_value(L, 3, sym, -1, &ct, 1);
	return 0;
}

static int jit_gc(lua_State* L)
{
	size_t i;
	JIT* jit = get_jit(L);
	dasm_free(jit);
	for (i = 0; i < jit->pagenum; i++) {
		FreePage(jit->pages[i], jit->pages[i]->size);
	}
	free(jit->pages);
	free(jit->globals);
	return 0;
}

static int ffi_debug(lua_State* L)
{
	lua_newtable(L);
	pushRegistry(L, &ctype_mt_key);
	lua_setfield(L, -2, "ctype_mt");
	pushRegistry(L, &cdata_mt_key);
	lua_setfield(L, -2, "cdata_mt");
	pushRegistry(L, &cmodule_mt_key);
	lua_setfield(L, -2, "cmodule_mt");
	pushRegistry(L, &constants_key);
	lua_setfield(L, -2, "constants");
	pushRegistry(L, &types_key);
	lua_setfield(L, -2, "types");
	pushRegistry(L, &jit_key);
	lua_setfield(L, -2, "jit");
	pushRegistry(L, &gc_key);
	lua_setfield(L, -2, "gc");
	pushRegistry(L, &callbacks_key);
	lua_setfield(L, -2, "callbacks");
	pushRegistry(L, &functions_key);
	lua_setfield(L, -2, "functions");
	pushRegistry(L, &abi_key);
	lua_setfield(L, -2, "abi");
	pushRegistry(L, &next_unnamed_key);
	lua_setfield(L, -2, "next_unnamed");
	return 1;
}

static int do64(lua_State* L, int is_unsigned)
{
	lua_Number low, high;
	CType ct;
	int64_t val;

	lua_settop(L, 2);

	if (!lua_isnil(L, 2)) {
		high = luaL_checknumber(L, 1);
		low = luaL_checknumber(L, 2);
	} else {
		high = 0;
		low = luaL_checknumber(L, 1);
	}

	val = ((int64_t) (uint32_t) high << 32) | (int64_t) (uint32_t) low;

	if (!is_unsigned && (high < 0 || low < 0)) {
		val = -val;
	}

	memset(&ct, 0, sizeof(ct));
	ct.type = INT64_TYPE;
	ct.is_unsigned = is_unsigned;
	ct.is_defined = 1;
	ct.base_size = sizeof(int64_t);
	push_number(L, (int64_t) val, 0, &ct);

	return 1;
}

static int ffi_i64(lua_State* L) {
	return do64(L, 0);
}

static int ffi_u64(lua_State* L) {
	return do64(L, 1);
}

#if 1
//how big is the main stack?
// same as lua_gettop(L) outside of any function ...
// but how to access that when all we are doing here is providing Lua functions etc to call?
#include <signal.h>
#define l_signalT	sig_atomic_t
typedef unsigned char lu_byte;
typedef signed char ls_byte;
typedef struct global_State global_State;
#if LUAI_IS32INT
typedef unsigned int l_uint32;
#else
typedef unsigned long l_uint32;
#endif
typedef l_uint32 Instruction;
typedef struct UpVal UpVal;
typedef struct GCObject GCObject;
typedef struct lua_longjmp lua_longjmp;
typedef union Value {
  struct GCObject *gc;    /* collectable objects */
  void *p;         /* light userdata */
  lua_CFunction f; /* light C functions */
  lua_Integer i;   /* integer numbers */
  lua_Number n;    /* float numbers */
  /* not used, but may avoid warnings for uninitialized value */
  lu_byte ub;
} Value;
typedef struct TValue {
  Value value_; lu_byte tt_;
} TValue;
typedef union StackValue {
  TValue val;
  struct {
    Value value_; lu_byte tt_;
    unsigned short delta;
  } tbclist;
} StackValue;
typedef StackValue *StkId;
typedef union {
  StkId p;  /* actual pointer */
  ptrdiff_t offset;  /* used while the stack is being reallocated */
} StkIdRel;
typedef struct CallInfo {
  StkIdRel func;  /* function index in the stack */
  StkIdRel	top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      const Instruction *savedpc;
      volatile l_signalT trap;  /* function is tracing lines/counts */
      int nextraargs;  /* # of extra arguments in vararg functions */
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  union {
    int funcidx;  /* called-function index */
    int nyield;  /* number of values yielded */
    int nres;  /* number of values returned */
    struct {  /* info about transferred values (for call/return hooks) */
      unsigned short ftransfer;  /* offset of first value transferred */
      unsigned short ntransfer;  /* number of values transferred */
    } transferinfo;
  } u2;
  short nresults;  /* expected number of results from this function */
  unsigned short callstatus;
} CallInfo;
struct lua_State {
  struct GCObject *next; lu_byte tt; lu_byte marked;
  lu_byte status;
  lu_byte allowhook;
  unsigned short nci;  /* number of items in 'ci' list */
  StkIdRel top;  /* first free slot in the stack */
  global_State *l_G;
  CallInfo *ci;  /* call info for current function */
  StkIdRel stack_last;  /* end of stack (last element + 1) */
  StkIdRel stack;  /* stack base */
  UpVal *openupval;  /* list of open upvalues in this stack */
  StkIdRel tbclist;  /* list of to-be-closed variables */
  GCObject *gclist;
  struct lua_State *twups;  /* list of threads with open upvalues */
  struct lua_longjmp *errorJmp;  /* current error recover point */
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
  volatile lua_Hook hook;
  ptrdiff_t errfunc;  /* current error handling function (stack index) */
  l_uint32 nCcalls;  /* number of nested (non-yieldable | C)  calls */
  int oldpc;  /* last pc traced */
  int basehookcount;
  int hookcount;
  volatile l_signalT hookmask;
};
static int ffi_stack(lua_State * L) {
	lua_pushinteger(L, L->top.p - L->stack.p);
	return 1;
}
#else
static int ffi_stack(lua_State * L) { lua_gettop(L); return 1; }	// lol always 0
#endif

static const luaL_Reg cdata_mt[] = {
	{"__gc", cdata_gc},
	{"__call", cdata_call},
	{"free", cdata_free},
	{"set", cdata_set},
	{"__index", cdata_index},
	{"__newindex", cdata_newindex},
	{"__add", cdata_add},
	{"__sub", cdata_sub},
	{"__mul", cdata_mul},
	{"__div", cdata_div},
	{"__mod", cdata_mod},
	{"__pow", cdata_pow},
	{"__unm", cdata_unm},
	{"__idiv", cdata_idiv},
	{"__band", cdata_band},
	{"__bor", cdata_bor},
	{"__bxor", cdata_bxor},
	{"__bnot", cdata_bnot},
	{"__shl", cdata_shl},
	{"__shr", cdata_shr},
	{"__eq", cdata_eq},
	{"__lt", cdata_lt},
	{"__le", cdata_le},
	{"__tostring", cdata_tostring},
	{"__concat", cdata_concat},
	{"__len", cdata_len},
	{"__pairs", cdata_pairs},
	{"__ipairs", cdata_ipairs},
	{NULL, NULL}
};

static const luaL_Reg callback_mt[] = {
	{"__gc", callback_free},
	{NULL, NULL}
};

static const luaL_Reg ctype_mt[] = {
	{"__call", ctype_call},
	{"__new", ctype_new},
	{"__tostring", ctype_tostring},
	{"__index", ctype_index},
	{"__eq", ctype_eq},
	{NULL, NULL}
};

static const luaL_Reg cmodule_mt[] = {
	{"__index", cmodule_index},
	{"__newindex", cmodule_newindex},
	{NULL, NULL}
};

static const luaL_Reg jit_mt[] = {
	{"__gc", jit_gc},
	{NULL, NULL}
};

static const luaL_Reg ffi_reg[] = {
	// Original LuaJIT:
	{"string", ffi_string},
	{"gc", ffi_gc},
	{"metatype", ffi_metatype},
	{"abi", ffi_abi},
	{"fill", ffi_fill},
	{"copy", ffi_copy},
	{"errno", ffi_errno},
	{"offsetof", ffi_offsetof},
	{"alignof", ffi_alignof},
	{"istype", ffi_istype},
	{"typeof", ffi_typeof},
	{"cast", ffi_cast},
	{"new", ffi_new},
	{"cdef", ffi_cdef},
	{"sizeof", ffi_sizeof},
	{"load", ffi_load},
	//Original defined elsewhere: C arch os
	//Originals missing: typeinfo

	// Extra to LuaFFIFB:
	{"debug", ffi_debug},
	{"i64", ffi_i64},
	{"u64", ffi_u64},

	// Chris' debugging:
	{"stack", ffi_stack},

	{NULL, NULL}
};

/*
Fills out ctype with args passed.
Creates userdata of the ctype, gives it a uservalue of {} if it IS_COMPLEX
Sets registry[&types_key] = the userdata of ctype
Leaves the stack the same.
*/
static void push_builtin(
	lua_State* L,
	CType* ct,
	const char* name,
	int type,
	int size,
	int align,
	int is_unsigned
) {
									// stack: ...
	memset(ct, 0, sizeof(*ct));
	ct->type = type;
	ct->base_size = size;
	ct->align_mask = align;
	ct->is_defined = 1;
	ct->is_unsigned = is_unsigned;

	if (IS_COMPLEX(type)) {
		lua_newtable(L);			// stack: ..., ctype uservalue = {}
	} else {
		lua_pushnil(L);				// stack: ..., ctype uservalue = nil
	}

	pushRegistry(L, &types_key);	// stack: ..., ctype uservalue, registry[&types_key]
	push_ctype(L, -2, ct);			// stack: ..., ctype uservalue, registry[&types_key], userdata of ct
	lua_setfield(L, -2, name);		// stack: ..., ctype uservalue, registry[&types_key];  registry[&types_key][name] = userdata of ct
	lua_pop(L, 2); 					// stack: ...
}

/*
Same exact thing as push_builtin except size, align, is_unsigned default to zero.
Except here no uservalue is ever created for the ct userdata copy, even if the type is complex.
*/
inline void push_builtin_undef(
	lua_State* L,
	CType* ct,
	const char* name,
	int type
) {									// stack: ...
	memset(ct, 0, sizeof(*ct));
	ct->type = type;

	pushRegistry(L, &types_key);	// stack: ..., registry[&types_key]
	push_ctype(L, 0, ct);			// stack: ..., registry[&types_key], userdata of ct
	lua_setfield(L, -2, name);		// stack: ..., registry[&types_key];  registry[&types_key][name] = userdata of ct
	lua_pop(L, 1);					// stack: ...
}

/*
Adds a new entry in registry[&types_key][to] that points to a new ctype userdata that represents whatever was parsed from `from`.
Leaves the stack the same.
*/
static void add_typedef(
	lua_State* L,
	const char* from,
	const char* to
) {												// stack: ...
	Parser P = newParser(L, from, 0); // 0 == don't use type params, since this is just used internally here for some quick typedefs and there's no $'s in the typedef strings

	pushRegistry(L, &types_key);				// stack: ..., registry[&types_key]
	CType ct = parse_type(L, &P);						// stack: ..., registry[&types_key], ctype uservalue[1]
	parse_argument(L, &P, -1, &ct, NULL, NULL);	// stack: ..., registry[&types_key], ctype uservalue[1], arg??? uservalue[1]
	push_ctype(L, -1, &ct);						// stack: ..., registry[&types_key], ctype uservalue[1], arg uservalue[1], userdata copy of ct

	lua_setfield(L, -4, to);					// stack: ..., registry[&types_key], ctype uservalue[1], arg uservalue[1];  registry[&types_key][to] = userdata copy of ct
	lua_pop(L, 3);								// stack: ...
}

/*
I wonder why this function is separate, it's all only ever called upon luaopen_ffi
This initializes the ffi table.
stack in: registry[&ffi_key]
*/
static int ffiInit(lua_State* L) {
	JIT* jit = get_jit(L);

	// jit setup
	{
		dasm_init(jit, 64);
#ifdef _WIN32
		{
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			jit->align_page_size = si.dwAllocationGranularity - 1;
		}
#else
		jit->align_page_size = sysconf(_SC_PAGE_SIZE) - 1;
#endif
		jit->globals = (void**) malloc(64 * sizeof(void*));
		dasm_setupglobal(jit, jit->globals, 64);
		compile_globals(jit, L);
	}

	// ffi.C
	{
#ifdef _WIN32
		size_t sz = sizeof(HMODULE) * 6;
		HMODULE* libs = lua_newuserdata(L, sz);
		memset(libs, 0, sz);

		// exe
		GetModuleHandle(NULL);
		// lua dll
#ifdef LUA_DLL_NAME
#define STR2(tok) #tok
#define STR(tok) STR2(tok)
		libs[1] = LoadLibraryA(STR(LUA_DLL_NAME));
#undef STR
#undef STR2
#endif

		// crt
#ifdef UNDER_CE
		libs[2] = LoadLibraryA("coredll.dll");
#else
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (char*) &_fmode, &libs[2]);
		libs[3] = LoadLibraryA("kernel32.dll");
		libs[4] = LoadLibraryA("user32.dll");
		libs[5] = LoadLibraryA("gdi32.dll");
#endif

		jit->lua_dll = libs[1];
		jit->kernel32_dll = libs[3];

#else // !_WIN32
		size_t sz = sizeof(void*) * 5;
		void** libs = (void**)lua_newuserdata(L, sz);
		memset(libs, 0, sz);

		libs[0] = LoadLibraryA(NULL); // exe
		libs[1] = LoadLibraryA("libc.so");
#ifdef __GNUC__
		libs[2] = LoadLibraryA("libgcc.so");
#endif
		libs[3] = LoadLibraryA("libm.so");
		libs[4] = LoadLibraryA("libdl.so");
#endif

											// stack: ffi, libs userdata of void*[]
		lua_newtable(L);					// stack: ffi, libs, t={}
		lua_setuservalue(L, -2);			// stack: ffi, libs;  set libs uservalue[1] to t

		pushRegistry(L, &cmodule_mt_key);	// stack: ffi, libs, registry[&cmodule_mt_key]
		lua_setmetatable(L, -2);			// stack: ffi, libs;  setmetatable(libs, registry[&cmodule_mt_key])

		lua_setfield(L, 1, "C");			// stack: ffi;  ffi.C = libs
	}

	// setup builtin types
	{
		struct {char ch; uint16_t v;} a16;
		struct {char ch; uint32_t v;} a32;
		struct {char ch; uint64_t v;} a64;
		struct {char ch; float v;} af;
		struct {char ch; double v;} ad;
#ifdef HAVE_LONG_DOUBLE
		struct {char ch; long double v;} ald;
#endif
		struct {char ch; uintptr_t v;} aptr;
		CType ct;
		struct {char ch; complex_float v;} cf;
		struct {char ch; complex_double v;} cd;
#if defined HAVE_LONG_DOUBLE && defined HAVE_COMPLEX
		struct {char ch; complex long double v;} cld;
#endif

		// assign registry[&types_key][name] = userdata of the ctype filled out, and for complex types assign a table uservalue to the ctype userdata
		push_builtin(L, &ct, "void", VOID_TYPE, 0, 0, 0);
		push_builtin(L, &ct, "bool", BOOL_TYPE, sizeof(_Bool), sizeof(_Bool) -1, 1);
		push_builtin(L, &ct, "uint8_t", INT8_TYPE, sizeof(uint8_t), 0, 1);
		push_builtin(L, &ct, "int8_t", INT8_TYPE, sizeof(int8_t), 0, 0);
		push_builtin(L, &ct, "uint16_t", INT16_TYPE, sizeof(uint16_t), ALIGNOF(a16), 1);
		push_builtin(L, &ct, "int16_t", INT16_TYPE, sizeof(int16_t), ALIGNOF(a16), 0);
		push_builtin(L, &ct, "uint32_t", INT32_TYPE, sizeof(uint32_t), ALIGNOF(a32), 1);
		push_builtin(L, &ct, "int32_t", INT32_TYPE, sizeof(int32_t), ALIGNOF(a32), 0);
		push_builtin(L, &ct, "uint64_t", INT64_TYPE, sizeof(uint64_t), ALIGNOF(a64), 1);
		push_builtin(L, &ct, "int64_t", INT64_TYPE, sizeof(int64_t), ALIGNOF(a64), 0);
		push_builtin(L, &ct, "float", FLOAT_TYPE, sizeof(float), ALIGNOF(af), 0);
		push_builtin(L, &ct, "double", DOUBLE_TYPE, sizeof(double), ALIGNOF(ad), 0);
#ifdef HAVE_LONG_DOUBLE
		push_builtin(L, &ct, "long double", LONG_DOUBLE_TYPE, sizeof(long double), ALIGNOF(ald), 0);
#else
		push_builtin_undef(L, &ct, "long double", LONG_DOUBLE_TYPE);
#endif
		push_builtin(L, &ct, "uintptr_t", INTPTR_TYPE, sizeof(uintptr_t), ALIGNOF(aptr), 1);
		push_builtin(L, &ct, "intptr_t", INTPTR_TYPE, sizeof(uintptr_t), ALIGNOF(aptr), 0);
		push_builtin(L, &ct, "complex float", COMPLEX_FLOAT_TYPE, sizeof(complex_float), ALIGNOF(cf), 0);
		push_builtin(L, &ct, "complex double", COMPLEX_DOUBLE_TYPE, sizeof(complex_double), ALIGNOF(cd), 0);
#if defined HAVE_LONG_DOUBLE && defined HAVE_COMPLEX
		push_builtin(L, &ct, "complex long double", COMPLEX_LONG_DOUBLE_TYPE, sizeof(complex long double), ALIGNOF(cld), 0);
#else
		push_builtin_undef(L, &ct, "complex long double", COMPLEX_LONG_DOUBLE_TYPE);
#endif

		// add NULL and i to registry[&constants_key] table, which is one of the many lookups of a module's __index:

		pushRegistry(L, &constants_key);		// stack: ffi, registry[&constants_key]

		memset(&ct, 0, sizeof(ct));
		ct.type = VOID_TYPE;
		ct.is_defined = 1;
		ct.pointers = 1;
		ct.is_null = 1;

		// add ffi.C.NULL
		push_cdata(L, 0, &ct);					// stack: ffi, registry[&constants_key], userdata cdata<void*>(0)
		lua_setfield(L, -2, "NULL");			// stack: ffi, registry[&constants_key];  registry[&constants_key].NULL = cdata<void*>(0)

		// add ffi.NULL
		push_cdata(L, 0, &ct);					// stack: ffi, registry[&constants_key], userdata cdata<void*>(0)
		lua_setfield(L, 1, "NULL");				// stack: ffi, registry[&constants_key];  ffi.NULL = cdata<void*>(0)

		// add ffi.C.null
		push_cdata(L, 0, &ct);					// stack: ffi, registry[&constants_key], userdata cdata<void*>(0)
		lua_setfield(L, -2, "null");			// stack: ffi, registry[&constants_key];  registry[&cosntants_key].null = cdata<void*>(0)

		// add ffi.null
		push_cdata(L, 0, &ct);					// stack: ffi, registry[&constants_key], userdata cdata<void*>(0)
		lua_setfield(L, 1, "null");				// stack: ffi, registry[&constants_key];  ffi.null = cdata<void*>(0)

		memset(&ct, 0, sizeof(ct));
		ct.type = COMPLEX_DOUBLE_TYPE;
		ct.is_defined = 1;
		ct.base_size = sizeof(complex_double);
		complex_double * pc = (complex_double*) push_cdata(L, 0, &ct);	// stack: ffi, registry[&constants_key], complex_double(0,1)
#ifdef HAVE_COMPLEX
		*pc = 1i;
#else
		pc->real = 0;
		pc->imag = 1;
#endif
		lua_setfield(L, -2, "i");				// stack: ffi, registry[*constants_key];  registry[*constants_key].i = complex_double(0,1)

		lua_pop(L, 1); 							// stack: ffi
	}

	assert(lua_gettop(L) == 1);					// stack: ffi

	// setup builtin typedefs
	{
		add_typedef(L, "bool", "_Bool");

		if (sizeof(uint32_t) == sizeof(size_t)) {
			add_typedef(L, "uint32_t", "size_t");
			add_typedef(L, "int32_t", "ssize_t");
		} else if (sizeof(uint64_t) == sizeof(size_t)) {
			add_typedef(L, "uint64_t", "size_t");
			add_typedef(L, "int64_t", "ssize_t");
		}

		if (sizeof(int32_t) == sizeof(intptr_t)) {
			add_typedef(L, "int32_t", "intptr_t");
			add_typedef(L, "int32_t", "ptrdiff_t");
		} else if (sizeof(int64_t) == sizeof(intptr_t)) {
			add_typedef(L, "int64_t", "intptr_t");
			add_typedef(L, "int64_t", "ptrdiff_t");
		}

		if (sizeof(uint8_t) == sizeof(wchar_t)) {
			add_typedef(L, "uint8_t", "wchar_t");
		} else if (sizeof(uint16_t) == sizeof(wchar_t)) {
			add_typedef(L, "uint16_t", "wchar_t");
		} else if (sizeof(uint32_t) == sizeof(wchar_t)) {
			add_typedef(L, "uint32_t", "wchar_t");
		}

		if (sizeof(va_list) == sizeof(char*)) {
			add_typedef(L, "char*", "va_list");
		} else {
			char tmp[256];
			struct {char ch; va_list v;} av;
			sprintf(tmp, "struct {char data[%d] __attribute__((align(%d)));}", (int) sizeof(va_list), (int) ALIGNOF(av) + 1);
			add_typedef(L, tmp, "va_list");
		}

		add_typedef(L, "va_list", "__builtin_va_list");
		add_typedef(L, "va_list", "__gnuc_va_list");
	}

	assert(lua_gettop(L) == 1);						// stack: ffi

	// setup ABI params table
	pushRegistry(L, &abi_key);						// stack: ffi, registry[&abi_key]

#if defined ARCH_X86 || defined ARCH_ARM
	setFieldTrue(L, -1, "32bit");
#elif defined ARCH_X64 || defined ARCH_PPC64 || defined ARCH_WASM
	setFieldTrue(L, -1, "64bit");
#else
#error cannot determine ABI
#endif

#if defined ARCH_X86 || defined ARCH_X64 || defined ARCH_ARM || defined ARCH_PPC64 || defined ARCH_WASM
	setFieldTrue(L, -1, "le");
#else
#error cannot determine le
#endif

#if defined ARCH_X86 || defined ARCH_X64 || defined ARCH_PPC64 || defined ARCH_WASM
	setFieldTrue(L, -1, "fpu");
#elif defined ARCH_ARM
	setFieldTrue(L, -1, "softfp");
#else
#error cannot determine fpu
#endif
	lua_pop(L, 1); 								// stack: ffi


	// GC table - shouldn't pin cdata values
	pushRegistry(L, &gc_key);					// stack: ffi, registry[&gc_key]
	lua_newtable(L);							// stack: ffi, registry[&gc_key], t={}
	lua_pushliteral(L, "k");					// stack: ffi, registry[&gc_key], t, "k"
	lua_setfield(L, -2, "__mode");				// stack: ffi, registry[&gc_key], t;  t.__mode = "k"
	lua_setmetatable(L, -2);					// stack: ffi, registry[&gc_key];  setmetatable(registry[&gc_key], {__mode="k"})
	lua_pop(L, 1);								// stack: ffi


	// ffi.os
#if defined OS_CE
	setFieldStrLit(L, 1, "os", "WindowsCE");
#elif defined OS_WIN
	setFieldStrLit(L, 1, "os", "Windows");
#elif defined OS_OSX
	setFieldStrLit(L, 1, "os", "OSX");
#elif defined OS_LINUX
	setFieldStrLit(L, 1, "os", "Linux");
#elif defined OS_BROWSER
	setFieldStrLit(L, 1, "os", "Browser");
#elif defined OS_BSD
	setFieldStrLit(L, 1, "os", "BSD");
#elif defined OS_POSIX
	setFieldStrLit(L, 1, "os", "POSIX");
#else
	setFieldStrLit(L, 1, "os", "Other");
#endif


	// ffi.arch
#if defined ARCH_X86
	setFieldStrLit(L, 1, "arch", "x86");
#elif defined ARCH_X64
	setFieldStrLit(L, 1, "arch", "x64");
#elif defined ARCH_ARM
	setFieldStrLit(L, 1, "arch", "arm");
#elif defined ARCH_PPC64
	setFieldStrLit(L, 1, "arch", "ppc64");
#elif defined ARCH_WASM
	setFieldStrLit(L, 1, "arch", "wasm");	// TODO wasm32 vs wasm64 would be nice
#else
# error cannot determine arch
#endif

	assert(lua_gettop(L) == 1);
	return 0;	// function ends, stack gets cleared anyways ... why the assert?
}

/*
expects a table + upvalues on top of the stack.
populates it with 'mtfuncs', which are given all upvalues in closure.
pops all upvalues.
*/
static void setup_mt(lua_State* L, const luaL_Reg* mtfuncs, int upvals) {
												// stack: ..., mt, upvalues...
	lua_pushboolean(L, 1);						// stack: ..., mt, upvalues..., true
	lua_setfield(L, -upvals-2, "__metatable");	// stack: ..., mt, upvalues...;  mt.__metatable = true
	luaL_setfuncs(L, mtfuncs, upvals);			// stack: ..., mt;  mt[mtfuncs[i][0]] = mtfuncs[i][1] with upvalues in closure
}

// creates an empty table and assigns it to registry[key], leaving the stack how it was.
static void newEmptyRegistryTable(lua_State * L, void * key) {
	lua_newtable(L);							// stack: ..., t={}
	setRegistry(L, key);						// stack: ...;  registry[key]=t
}

int luaopen_ffi(lua_State* L) {
	lua_settop(L, 0);							// clear lua stack, how presumptuous of you that the caller wasn't needing anything on it ...

	newEmptyRegistryTable(L, &niluv_key);		// registry[niluv_key]={}

	lua_newtable(L);							// stack: t={}
	setup_mt(L, ctype_mt, 0);					// stack: t;  t filled with `ctype_mt`
	setRegistry(L, &ctype_mt_key);				// stack: empty;  registry[ctype_mt_key]=t

	newEmptyRegistryTable(L, &callbacks_key);	// registry[callbacks_key]={}
	newEmptyRegistryTable(L, &gc_key);			// registry[gc_key]={}

	lua_newtable(L);							// stack: t={}
	pushRegistry(L, &callbacks_key);			// stack: t, registry[callbacks_key]
	pushRegistry(L, &gc_key);					// stack: t, registry[callbacks_key], registry[gc_key]
	setup_mt(L, cdata_mt, 2);					// stack: t;  t filled with `cdata_mt` with two upvalues: registry[callbacks_key], registry[gc_key]
	setRegistry(L, &cdata_mt_key);				// stack: empty;  registry[cdata_mt_key]=t

	lua_newtable(L);							// stack: t={}
	setup_mt(L, callback_mt, 0);				// stack: t;  t filled with `callback_mt`
	setRegistry(L, &callback_mt_key);			// stack: empty;  registry[callback_mt_key]=t

	lua_newtable(L);							// stack: t={}
	setup_mt(L, cmodule_mt, 0);					// stack: t;  t filled with `cmodule_mt`
	setRegistry(L, &cmodule_mt_key);			// stack: empty;  registry[cmodule_mt_key]=t

	memset(lua_newuserdata(L, sizeof(JIT)), 0, sizeof(JIT)); // stack: u=userdata of JIT
	lua_newtable(L);							// stack: u, t={}
assert(lua_gettop(L) == 2);
	setup_mt(L, jit_mt, 0);						// stack: u, t;  t filled with `jit_mt`
assert(lua_gettop(L) == 2);
	lua_setmetatable(L, -2);					// stack: u;  setmetatable(u, t)
	setRegistry(L, &jit_key);					// stack: empty;  registry[jit_key] = u;

	newEmptyRegistryTable(L, &constants_key);	// registry[constants_key]={}
	newEmptyRegistryTable(L, &types_key);		// registry[types_key]={}
	newEmptyRegistryTable(L, &functions_key);	// registry[functions_key]={}
	newEmptyRegistryTable(L, &asmname_key);		// registry[asmname_key]={}
	newEmptyRegistryTable(L, &abi_key);			// registry[abi_key]={}

	lua_pushinteger(L, 1);						// stack: 1
	setRegistry(L, &next_unnamed_key);			// stack: empty;  registry[&next_unnamed_key]=1

	assert(lua_gettop(L) == 0);

	lua_newtable(L);							// stack: ffi={}
	luaL_setfuncs(L, ffi_reg, 0);				// stack: ffi;  ffi filled with `ffi_reg`

	lua_pushcfunction(L, &ffiInit);				// stack: ffi, ffiInit
	lua_pushvalue(L, 1);						// stack: ffi, ffiInit, ffi
	lua_call(L, 1, 0);							// stack: ffi;  call `ffiInit(ffi)` ... why call instead of just calling the function from C?  is it for error handling? or am I thinking too deep about this?

	assert(lua_gettop(L) == 1);

	// replace tonumber function
	lua_getglobal(L, "tonumber");				// stack: ffi, tonumber
	lua_pushcclosure(L, &ffi_number, 1);		// stack: ffi, ffi_number;  ffi_number's upvalue 1 is the old `tonumber`
	lua_pushvalue(L, -1);						// stack: ffi, ffi_number, ffi_number
	lua_setglobal(L, "tonumber");				// stack: ffi, ffi_number;  _G.tonumber = ffi_number
	lua_setfield(L, -2, "number");				// stack: ffi;  ffi.number = ffi_number

	// replace type function
	lua_getglobal(L, "type");					// stack: ffi, type
	lua_pushcclosure(L, &ffi_type, 1);			// stack: ffi, ffi_type;  ffi_type's upvalue 1 is the old `type`
	lua_pushvalue(L, -1);						// stack: ffi, ffi_type, ffi_type
	lua_setglobal(L, "type");					// stack: ffi, ffi_type;  _G.type = ffi_type
	lua_setfield(L, -2, "type");				// stack: ffi;  ffi.type = ffi_type

	return 1;
}
