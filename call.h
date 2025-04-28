#pragma once

#include "lua.h"	//lua_State.h
#include "types.h"	//CFunction
#include "ffi.h"	//CType, JIT

#define DASM_EXTERN(a,b,c,d) get_extern(a,b,c,d)

// Defined here because it's forward-declared in ffi.h for JIT's definition
//  so it can be used in ffi.c and call.c
typedef struct Page {
	size_t size;
	size_t off;
	size_t freed;
} Page;

void compile_globals(JIT* jit, lua_State* L);
CFunction compile_callback(lua_State* L, int fidx, int ct_usr, const CType* ct);
void compile_function(lua_State* L, CFunction f, int ct_usr, const CType* ct);
int get_extern(JIT* jit, uint8_t* addr, int idx, int type);
void push_func_ref(lua_State* L, CFunction func);
void free_code(JIT* jit, lua_State* L, CFunction func);

// used in ffi.c 
// and in call_x86.dasc, which generates call_x86.h call_x64.h call_x64win.h 
// and all those are included in call.c
// so I'll put it here in call.h
#define CALLBACK_FUNC_USR_IDX 1

// used in call_x64.dasc and its generated headers
// and dynasm/dasm_arm.h dynasm/dasm_x86.h dynasm/dasm_ppc.h dynasm/dasm_proto.h
// so same once again, it goes here
struct JIT;
#define Dst_DECL	struct JIT* Dst
#define Dst_REF		(Dst->ctx)
