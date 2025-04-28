#pragma once

#include "types.h"	//CFunction
#include "ffi.h"	//CType, struct jit

#define DASM_EXTERN(a,b,c,d) get_extern(a,b,c,d)

// Defined here because it's forward-declared in ffi.h for struct jit's definition
//  so it can be used in ffi.c and call.c
typedef struct Page {
	size_t size;
	size_t off;
	size_t freed;
} Page;

void compile_globals(struct jit* jit, lua_State* L);
CFunction compile_callback(lua_State* L, int fidx, int ct_usr, const CType* ct);
void compile_function(lua_State* L, CFunction f, int ct_usr, const CType* ct);
int get_extern(struct jit* jit, uint8_t* addr, int idx, int type);
void push_func_ref(lua_State* L, CFunction func);
void free_code(struct jit* jit, lua_State* L, CFunction func);
