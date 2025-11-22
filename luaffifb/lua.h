#pragma once

#ifdef __cplusplus
extern "C" {
# include "lua5.4/lua.h"
# include "lua5.4/lauxlib.h"
# include "lua5.4/lualib.h"
}
#else
# include "lua5.4/lua.h"
# include "lua5.4/lauxlib.h"
# include "lua5.4/lualib.h"
#endif

int lua_absindex2(lua_State* L, int idx);
// use our own version of lua_absindex such that lua_absindex(L, 0) == 0
#define lua_absindex(L, idx) lua_absindex2(L, idx)

#if LUA_VERSION_NUM == 501
void lua_callk(lua_State *L, int nargs, int nresults, int ctx, lua_CFunction k);
/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup);

#define lua_setuservalue lua_setfenv
#define lua_getuservalue lua_getfenv
#define lua_rawlen lua_objlen
char* luaL_prepbuffsize(luaL_Buffer* B, size_t sz);

#elif LUA_VERSION_NUM >= 503

void (lua_remove)(lua_State *L, int idx);

#endif
