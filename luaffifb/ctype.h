#pragma once

#include "luaffifb/ffi.h"	// CType
#include "luaffifb/types.h"	// CFunction

void set_defined(lua_State* L, int ct_usr, CType* ct);

CType* push_ctype(lua_State* L, int ct_usr, const CType* ct);

size_t ctype_size(lua_State* L, const CType* ct);

void* push_cdata(lua_State* L, int ct_usr, const CType* ct); /* called from asm */

void push_callback(lua_State* L, CFunction luafunc, CFunction cfunc);
void check_ctype(lua_State* L, int idx, CType* ct, int typeParamStartLoc);

void* to_cdata(lua_State* L, int idx, CType* ct);
void* check_cdata(lua_State* L, int idx, CType* ct);
