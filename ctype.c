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

static int to_define_key;

/*
ct_usr is ...
	for push_builtin -> push_ctype -> here, it is {} for IS_COMPLEX(type), nil otherwise.
ct_idx is the stack location of the key of stack[ct_usr][&to_define_key]'s table to set.

Creates stack[ct_usr][&to_define_key] if it doesn't exist.
Assigns stack[ct_usr][&to_define_key][stack[ct_idx]] = true

Assumes stack[ct_usr] is not nil.
*/
static void update_on_definition(
	lua_State* L,
	int ct_usr,
	int ct_idx
) {													// stack: ...
	ct_usr = lua_absindex(L, ct_usr);
	ct_idx = lua_absindex(L, ct_idx);

	lua_pushlightuserdata(L, &to_define_key);		// stack: ..., &to_define_key
	lua_rawget(L, ct_usr);							// stack: ..., t=stack[ct_usr][&to_define_key]

	// if stack[ct_usr][&to_define_key] is nil then create it ...
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);								// stack: ...
		lua_newtable(L);							// stack: ..., t={}

		lua_newtable(L);							// stack: ..., t, mt={}
		lua_pushliteral(L, "k");					// stack: ..., t, mt, "k"
		lua_setfield(L, -2, "__mode");				// stack: ..., t, mt;  mt.__mode = "k"

		lua_setmetatable(L, -2);					// stack: ..., t;  setmetatable(t, mt)

		lua_pushlightuserdata(L, &to_define_key);	// stack: ..., t, u = &to_define_key
		lua_pushvalue(L, -2);						// stack: ..., t, u, t
		lua_rawset(L, ct_usr);						// stack: ..., t;  stack[ct_usr][&to_define_key] = t
	}

	// to_update[ctype or cdata] = true
	lua_pushvalue(L, ct_idx);						// stack: ..., t, stack[ct_idx]
	lua_pushboolean(L, 1);							// stack: ..., t, stack[ct_idx], true
	lua_rawset(L, -3);								// stack: ..., t;  t[stack[ct_idx]] = true

	lua_pop(L, 1);									// stack: ...
}

void set_defined(lua_State* L, int ct_usr, CType* ct) {
	ct_usr = lua_absindex(L, ct_usr);

	ct->is_defined = 1;

	/* update ctypes and cdatas that were created before the definition came in */
	lua_pushlightuserdata(L, &to_define_key);
	lua_rawget(L, ct_usr);

	if (!lua_isnil(L, -1)) {
		lua_pushnil(L);

		while (lua_next(L, -2)) {
			CType* upd = (CType*) lua_touserdata(L, -2);
			upd->base_size = ct->base_size;
			upd->align_mask = ct->align_mask;
			upd->is_defined = 1;
			upd->is_variable_struct = ct->is_variable_struct;
			upd->variable_increment = ct->variable_increment;
			assert(!upd->variable_size_known);
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
		/* usr[TO_UPDATE_KEY] = nil */
		lua_pushlightuserdata(L, &to_define_key);
		lua_pushnil(L);
		lua_rawset(L, ct_usr);
	} else {
		lua_pop(L, 1);
	}
}

/*
Creates a new `CType` userdata,
Copies its contents from `ct`,
Sets its metatable to registry[ctype_mt_key],
If `ct_usr` is nonzero then assigns it's 0th uservalue to `stack[ct_usr]`
- calling from push_builtin, for IS_COMPLEX(type), this is a {}, otherwise it is nil
*/
CType* push_ctype(
	lua_State* L,
	int ct_usr,
	const CType* ct
) {										// stack: ...
	ct_usr = lua_absindex(L, ct_usr);

	CType * ret = (CType *)lua_newuserdata(L, sizeof(CType));
	*ret = *ct;							// stack: ..., u = userdata of CType

	pushRegistry(L, &ctype_mt_key);		// stack: ..., u, registry[ctype_mt_key]
	lua_setmetatable(L, -2);			// stack: ..., u;  setmetatable(u, registry[ctype_mt_key])

#if LUA_VERSION_NUM == 501
	if (!ct_usr || lua_isnil(L, ct_usr)) {
		pushRegistry(L, &niluv_key);
		lua_setfenv(L, -2);
	}
#endif

	if (ct_usr && !lua_isnil(L, ct_usr)) {
		lua_pushvalue(L, ct_usr);		// stack: ..., u, stack[ct_usr]
		lua_setuservalue(L, -2);		// stack: ..., u;  u's uservalue[1] is set to stack[ct_usr]
	}

	// if stack[ct_usr] is not nil then set stack[ct_usr][&to_define_key][u] = true
	//  why we do this, idk.
	if (!ct->is_defined && ct_usr && !lua_isnil(L, ct_usr)) {
		update_on_definition(L, ct_usr, -1);
	}

	return ret;							// stack: ..., u
}

size_t ctype_size(lua_State* L, const CType* ct) {
	if (ct->pointers - ct->is_array) {
		return sizeof(void*) * (ct->is_array ? ct->array_size : 1);

	} else if (!ct->is_defined || ct->type == VOID_TYPE) {
		return luaL_error(L, "can't calculate size of an undefined type");

	} else if (ct->variable_size_known) {
		assert(ct->is_variable_struct && !ct->is_array);
		return ct->base_size + ct->variable_increment;

	} else if (ct->is_variable_array || ct->is_variable_struct) {
		return luaL_error(L, "internal error: calc size of variable type with unknown size");

	} else {
		return ct->base_size * (ct->is_array ? ct->array_size : 1);
	}
}

/*
Creates a new `CData` userdata & leaves it on the stack,
Sets its metatable to registry[cdata_mt_key]
If `ct_usr` is nonzero then assigns it's 0th uservalue to `stack[ct_usr]`
... what is the uservalue of cdata userdata supposed to be?  How come I get the feeling it varies depending on the type ...
Returns the pointer past the cdata to hold the cdata's contents.
*/
void * push_cdata(
	lua_State * L,
	int ct_usr,
	CType const * ct
) {										// stack: ...
	ct_usr = lua_absindex(L, ct_usr);

	size_t sz = ct->is_reference ? sizeof(void*) : ctype_size(L, ct);

	/* This is to stop valgrind from complaining. Bitfields are accessed in 8
	 * byte chunks so that the code doesn't have to deal with different access
	 * patterns, but this means that occasionally it will read past the end of
	 * the struct. As its not setting the bits past the end (only reading and
	 * then writing the bits back) and the read is aligned its a non-issue,
	 * but valgrind complains nonetheless.
	 */
	if (ct->has_bitfield) {
		sz = ALIGN_UP(sz, 7);
	}

	CData * cd = (CData *)lua_newuserdata(L, sizeof(CData) + sz);
										// stack: ..., u = userdata of {CData, byte[sz]}
	cd->type = *ct;
	memset(cd+1, 0, sz);

	/* TODO: handle cases where lua_newuserdata returns a pointer that is not aligned */
#if 0
	assert((uintptr_t) (cd + 1) % 8 == 0);
#endif

#if LUA_VERSION_NUM == 501
	if (!ct_usr || lua_isnil(L, ct_usr)) {
		pushRegistry(L, &niluv_key);
		lua_setfenv(L, -2);
	}
#endif

	if (ct_usr && !lua_isnil(L, ct_usr)) {
		lua_pushvalue(L, ct_usr);		// stack: ..., u, stack[ct_usr]
		lua_setuservalue(L, -2);		// stack: ..., u;  u's uservalue[1] is set to stack[ct_usr]
	}

	pushRegistry(L, &cdata_mt_key);		// stack: ..., u, registry[&cdata_mt_key]
	lua_setmetatable(L, -2);			// stack: ..., u;  setmetatable(u, registry[&cdata_mt_key])

	// if stack[ct_usr] is not nil then set stack[ct_usr][&to_define_key][u] = true
	//  why we do this, idk.
	if (!ct->is_defined && ct_usr && !lua_isnil(L, ct_usr)) {
		update_on_definition(L, ct_usr, -1);
	}

	return cd+1;
}

// Pushes onto the stack a userdata of CFunction[2]={luafunc, cfunc} with metatable registry[&callback_mt]
void push_callback(
	lua_State * L,
	CFunction luafunc,
	CFunction cfunc
) {										// stack: ...
	CFunction * pf = (CFunction *)lua_newuserdata(L, 2 * sizeof(CFunction));
										// stack: ..., pf = userdata of CFunction[2]
	pf[0] = luafunc;
	pf[1] = cfunc;

	pushRegistry(L, &callback_mt_key);	// stack: ..., pf, registry[&callback_mt_key]
	lua_setmetatable(L, -2);			// stack: ..., pf;  setmetatable(pf, registry[&callback_mt_key])
}

/*
Looks at the stack index `idx`,
If it's a string then parses it.
If it's a ctype or cdata metatable then uses the associated ctype.
Writes the ctype to `ct`.
Pushes the CType's userdata's uservalue[1] onto the stack ...
... for ctype, this is some weird arg, either {} for complex or nil
... for cdata, what is this?
And how come it's casting cdata_mt_key's as CType?
*/
void check_ctype(
	lua_State* L,
	int idx,
	CType * ct	// out
) {											// stack: ...
	if (lua_isstring(L, idx)) {
		// first, replace all $'s with names of ctypes
		for (;;) {
			char const *ctypename = lua_tostring(L, idx);
//printf("ctypename %s\n", ctypename);
			size_t ctypenamelen = strlen(ctypename);
//printf("ctypenamelen %ld\n", ctypenamelen);
			char *loc = strchr(ctypename, '$');
			if (!loc) break;
			size_t locindex = loc - ctypename;
//printf("locindex %ld\n", locindex);
			//check_ctype is always called with idx==1
			//so start looking at idx==2 for our type args
			//extra bonus points for this check_ctype to assert it is not a string
			CType ctarg;
			check_ctype(L, idx+1, &ctarg);
			push_type_name(L, idx+1, &ctarg);

			char const *argname = lua_tostring(L, -1);
//printf("argname %s\n", argname);
			size_t argnamelen = strlen(argname);
//printf("argnamelen %ld\n", argnamelen);
			char * fixednamebuf = malloc(ctypenamelen + argnamelen + 32);
			memcpy(fixednamebuf, ctypename, locindex);
			memcpy(fixednamebuf + locindex, argname, argnamelen);
			memcpy(fixednamebuf + locindex + argnamelen, ctypename + locindex + 1, ctypenamelen - (locindex + 1));
			fixednamebuf[ctypenamelen - 1 + argnamelen] = '\0';

			lua_pop(L, 1);			// remove argname
			lua_remove(L, idx+1);	// remove ctype that created argname

//printf("fixedname %s\n", fixednamebuf);
			lua_pushstring(L, fixednamebuf);
			lua_replace(L, idx);
			free(fixednamebuf);
		}

		Parser P = newParser(lua_tostring(L, idx));
		parse_type(L, &P, ct);						// stack: ..., ct's userdata's uservalue[1]
		parse_argument(L, &P, -1, ct, NULL, NULL);	// stack: ..., ctype uservalue, ... arg uservalue or new ctype uservalue which is it?
		lua_remove(L, -2); 							// stack: ..., parse_argument returned uservalue
		return;
	}

	if (lua_getmetatable(L, idx)
		&& (
			equalsRegistry(L, -1, &ctype_mt_key)
			|| equalsRegistry(L, -1, &cdata_mt_key)
		)
	) {													// stack: ..., getmetatable(stack[idx])
		lua_pop(L, 1); 									// stack: ...
		// wait ... if it's a cdata ... then treat its userdata as a struct type ... why?
		*ct = *(CType*)lua_touserdata(L, idx);	// stack: ...
		lua_getuservalue(L, idx);						// stack: ..., stack[idx]'s uservalue[1]
		return;
	}

	luaL_error(L, "expected cdata, ctype or string for arg #%d", idx);
}

/*
Pushes stack[idx]'s uservalue[1] (i.e. the CType's userdata's uservalue[1]) onto the stack.
Returns the void* associated with the CData, (i.e. ((CData*)ptr)+1, except references or non-array pointers )
If the index is not a ctype then ct is set to the zero value such that ct->type is INVALID_TYPE, a nil is pushed, and NULL is returned.
*/
void * to_cdata(
	lua_State* L,
	int idx,
	CType * ctype	// out
) {													// stack: ...
	// Chris: If we always returned cd+1 instead of dereferencing it for references, pointers, and arrays,
	// then the result of NULL can determine non-cdata, and this memset can be skipped for non-cdata values.
	memset(ctype, 0, sizeof(CType));

	if (!lua_isuserdata(L, idx) || !lua_getmetatable(L, idx)) {
		lua_pushnil(L);								// stack: ..., nil
		return NULL;
	}
													// stack: ..., getmetatable(stack[idx])
	if (!equalsRegistry(L, -1, &cdata_mt_key)) {
		lua_pop(L, 1);								// stack: ...
		lua_pushnil(L);								// stack: ..., nil
		return NULL;
	}

	lua_pop(L, 1);									// stack: ...
	CData * cd = (CData *)lua_touserdata(L, idx);
	*ctype = cd->type;
	lua_getuservalue(L, idx);						// stack: ..., stack[idx]'s uservalue[1]

	if (ctype->is_reference
		|| (ctype->pointers && !ctype->is_array)
	) {
		return *(void**) (cd+1);
	} else {
		return cd + 1;
	}
}

/*
Pushes the CData's userdata's uservalue[1] onto the stack
returns the CData*
*/
void * check_cdata(
	lua_State * L,
	int idx,
	CType * ct
) {											// stack: ...
	void * p = to_cdata(L, idx, ct);		// stack: ..., stack[idx]'s uservalue[1] if it is a CData, nil otherwise
	if (ct->type == INVALID_TYPE) {
		luaL_error(L, "expected cdata for arg #%d", idx);
	}
	return p;
}
