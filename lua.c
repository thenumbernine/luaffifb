#include "luaffifb/lua.h"

int lua_absindex2(lua_State* L, int idx) {
	return (LUA_REGISTRYINDEX <= idx && idx < 0)
		 ? lua_gettop(L) + idx + 1
		 : idx;
}

#if LUA_VERSION_NUM == 501
void lua_callk(lua_State *L, int nargs, int nresults, int ctx, lua_CFunction k) {
	lua_call(L, nargs, nresults);
}
void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup) {
	luaL_checkstack(L, nup, "too many upvalues");
	for (; l && l->name; l++) {	// fill the table with given functions
		int i;
		for (i = 0; i < nup; i++)	// copy upvalues to the top
			lua_pushvalue(L, -nup);
		lua_pushcclosure(L, l->func, nup);	// closure with those upvalues
		lua_setfield(L, -(nup + 2), l->name);
	}
	lua_pop(L, nup);	// remove upvalues
}

char* luaL_prepbuffsize(luaL_Buffer* B, size_t sz) {
	if (sz > LUAL_BUFFERSIZE) {
		luaL_error(B->L, "string too long");
	}
	return luaL_prepbuffer(B);
}

#elif LUA_VERSION_NUM >= 503

void (lua_remove)(lua_State *L, int idx) {
	lua_remove(L, idx);
}

#endif
