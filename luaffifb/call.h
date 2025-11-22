#pragma once

#include "luaffifb/lua.h"	//lua_State.h
#include "luaffifb/types.h"	//CFunction
#include "luaffifb/ffi.h"	//CType

struct Page;		// defined in call.h, used in call.c and ffi.c
struct DASMState;	// defined in dynasm/dasm_*.h

typedef struct JIT {
	lua_State* L;
	int32_t last_errno;
	struct DASMState* ctx;
	size_t pagenum;
	struct Page** pages;
	size_t align_page_size;
	void** globals;
	int function_extern;
	void* lua_dll;
	void* kernel32_dll;
} JIT;

#define DASM_EXTERN(a,b,c,d) get_extern(a,b,c,d)

// Defined here because it's forward-declared in ffi.h for JIT's definition
//  so it can be used in ffi.c and call.c
typedef struct Page {
	size_t size;
	size_t off;
	size_t freed;
} Page;

JIT* get_jit(lua_State* L);
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
#define Dst_DECL	JIT* Dst
#define Dst_REF		(Dst->ctx)


// Used with the LibFFI calls:

int callLuaToCWithLibFFI(lua_State *L);
