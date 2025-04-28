/* vim: ts=4 sw=4 sts=4 et tw=78
 * Portions copyright (c) 2015-present, Facebook, Inc. All rights reserved.
 * Portions copyright (c) 2011 James R. McKaskill.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include "ffi.h"

static CFunction compile(Dst_DECL, lua_State* L, CFunction func, int ref);

static void* reserve_code(JIT* jit, lua_State* L, size_t sz);
static void commit_code(JIT* jit, void* p, size_t sz);

static void push_int(lua_State* L, int val) { lua_pushinteger(L, val); }
static void push_uint(lua_State* L, unsigned int val) { lua_pushinteger(L, val); }
static void push_float(lua_State* L, float val) { lua_pushnumber(L, val); }

#ifndef _WIN32
static int GetLastError(void) { return errno; }
static void SetLastError(int err) { errno = err; }
#endif

#ifdef NDEBUG
#define shred(a,b,c)
#else
#define shred(p,s,e) memset((uint8_t*)(p)+(s),0xCC,(e)-(s))
#endif

#ifdef __wasm__

// wasm libffi compile_ goes here, not somewhere else, cuz I want to generate a diff patch
#include <ffi.h>

union Value {
	float f;
	double d;
	void * p;
	int64_t i;
};

struct CallInfo {
	ffi_cif cif;
	CFunction func;
	int nargs;
	void ** valuePtrs;	//allocated upon creation, size nargs, points into valueData
	Value * valueData;
};

void compile_globals(JIT* jit, lua_State* L) {}

static inline ffi_type * getFFITypeForCType(CType const * mbr_ct) {
	if (mbr_ct->pointers || mbr_ct->is_reference || mbr_ct->type == INTPTR_TYPE) {
		return &ffi_type_pointer;
	}
	switch (mbr_ct->type) {
	case FUNCTION_PTR_TYPE: return &ffi_type_pointer;
	case ENUM_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint32 : &ffi_type_sint32;
	case INT64_TYPE: return &ffi_type_sint64;
	case COMPLEX_FLOAT_TYPE: return &ffi_type_complex_float;
	case COMPLEX_DOUBLE_TYPE: return &ffi_type_complex_double;
	case VOID_TYPE: return &ffi_type_void;
	case BOOL_TYPE: return &ffi_type_int8;
	case INT8_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint8 : &ffi_type_sint8;
	case INT16_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint16 : &ffi_type_sint16;
	case INT32_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint32 : &ffi_type_sint32;
	case FLOAT_TYPE: return &ffi_type_float;
	case DOUBLE_TYPE: return &ffi_type_double;
	default:
		luaL_error(L, "NYI: call return type");
	}
}

/*
ok i've completely lost track of what is what ...
upvalues:
#1: whatever ct_usr is (the first upvalue of cdata_call?)
#2: CallInfo userdata
*/
static void call_ffi(lua_State *L) {
	int ct_usr = lua_upvalueindex(1);

#error TODO looks like I need to have the values[] point at the value itself, which I need to store somewhere else

	// get closure arg #1 as the ffi_cif
	CallInfo * callInfo = (CallInfo*)lua_touserdata(L, lua_upvalueindex(2));

	// translate all the Lua args into FFI args
	for (int i = 1; i <= callInfo->nargs; ++i) {
		lua_rawgeti(L, ct_usr, i);
		const CType * mbr_ct = (const CType*) lua_touserdata(L, -1);

		if (mbr_ct->pointers || mbr_ct->is_reference || mbr_ct->type == INTPTR_TYPE) {
			callInfo->valueData[i-1].i = cast_int64(L, i, 0);
		} else {
			switch (mbr_ct->type) {
			case FUNCTION_PTR_TYPE:
				callInfo->valueData[i-1].i = cast_int64(L, i, 0);
				break;
			case ENUM_TYPE:
				if (mbr_ct->is_unsigned) {
					callInfo->valueData[i-1].i = cast_uint32(L, i);
				} else {
					callInfo->valueData[i-1].i = cast_int32(L, i);
				}
				break;
			case COMPLEX_FLOAT_TYPE:
				callInfo->valueData[i-1].i = check_complex_float(L, i);
				break;
			case COMPLEX_DOUBLE_TYPE:
				callInfo->valueData[i-1].i = check_complex_double(L, i);
				break;
			case BOOL_TYPE:
				callInfo->valueData[i-1].i = (cast_int64(L, idx, !check_pointers) != 0);
				break;
			case INT8_TYPE:
			case INT16_TYPE:
			case INT32_TYPE:
			case INT64_TYPE:
				if (mbr_ct->is_unsigned) {
					callInfo->valueData[i-1].i = cast_uint64(L, i, 0);
				} else {
					callInfo->valueData[i-1].i = cast_int64(L, i, 0);
				}
				break;
			case FLOAT_TYPE:
				callInfo->valueData[i-1].f = check_double(L, i);
				break;
			case DOUBLE_TYPE:
				callInfo->valueData[i-1].d = check_double(L, i);
				break;
			default:
				luaL_error(L, "NYI: call return type");
			}
		}

		lua_pop(L, 1);
	}

	// do the call
	void *ret = {};
	ffi_call(callInfo->cif, callInfo->func, &ret, callInfo->valuePtrs);

	// TODO translate the Lua result to C result
}

CFunction compile_callback(lua_State* L, int fidx, int ct_usr, const CType* ct) {
	luaL_error(L, "TODO compile_callback");
	return {};
}

void compile_function(lua_State* L, CFunction func, int ct_usr, const CType* ct) {
	int top = lua_gettop(L);
	ct_usr = lua_absindex(L, ct_usr);

	if (ct->calling_convention != C_CALL && ct->has_var_arg) {
		luaL_error(L, "vararg is only allowed with the c calling convention");
	}

// what's this for?
//	void * p = push_cdata(L, ct_usr, ct);
//	*(CFunction*) p = func;

	// fill out types
	size_t nargs = lua_rawlen(L, ct_usr);
	const int maxArgs = 256;
	ffi_type * argFFITypes[maxArgs] = {NULL};
	if (nargs > maxArgs) {
		luaL_error(L, "function call has too many args: %d > %d\n", nargs, maxArgs);
	}

	for (int i = 1; i <= nargs; i++) {
		lua_rawgeti(L, ct_usr, i);
		const CType * mbr_ct = (const CType*) lua_touserdata(L, -1);
		argFFITypes[i-1] = getFFITypeForCType(mbr_ct);
		lua_pop(L, 1);
	}

	lua_rawgeti(L, ct_usr, 0);
	const CType * mbr_ct = (const CType*) lua_touserdata(L, -1);
	lua_pop(L, 1);

	ffi_type * retFFIType = getFFITypeForCType(mbr_ct);

	// push the ffi_cif
	lua_pushvalue(L, ct_usr);
	CallInfo * callinfo = (CallInfo*)lua_newuserdata(L, sizeof(CallInfo));
	callInfo->func = func;
	callInfo->nargs = nargs;
	callInfo->valueData = (Value*)malloc(sizeof(Value) * nargs);
	callInfo->valuePtrs = (void**)malloc(sizeof(void*) * nargs);
	for (int i = 0; i < nargs; ++i) {
		callInfo->valuePtrs[i] = callInfo->valueData + i;
	}

	ffi_status prepResult = ffi_prep_cif(&callInfo->cif, FFI_DEFAULT_ABI, nargs, retFFIType, argFFITypes);
	if (prepResult != FFI_OK) {
		luaL_error(L, "ffi_prep_cif failed with %d", prepResult);
	}

	// save it as a closure arg
	// push the call_ffi function
	lua_pushcclosure(L, call_ffi, 2);
}

#elif defined _WIN64
#include "dynasm/dasm_x86.h"
#include "call_x64win.h"
#elif defined __amd64__
#include "dynasm/dasm_x86.h"
#include "call_x64.h"
#elif defined __arm__ || defined __arm || defined __ARM__ || defined __ARM || defined ARM || defined _ARM_ || defined ARMV4I || defined _M_ARM
#include "dynasm/dasm_arm.h"
#include "call_arm.h"
#else
#include "dynasm/dasm_x86.h"
#include "call_x86.h"
#endif

typedef struct JIT_head {
	size_t size;
	int ref;
	uint8_t jump[JUMP_SIZE];
} JIT_head;

#define LINKTABLE_MAX_SIZE (sizeof(extnames) / sizeof(extnames[0]) * (JUMP_SIZE))

static CFunction compile(JIT* jit, lua_State* L, CFunction func, int ref)
{
	JIT_head* code;
	size_t codesz;
	int err;

	dasm_checkstep(jit, -1);
	if ((err = dasm_link(jit, &codesz)) != 0) {
		char buf[32];
		sprintf(buf, "%x", err);
		luaL_error(L, "dasm_link error %s", buf);
	}

	codesz += sizeof(JIT_head);
	code = (JIT_head*) reserve_code(jit, L, codesz);
	code->ref = ref;
	code->size = codesz;
	compile_extern_jump(jit, L, func, code->jump);

	if ((err = dasm_encode(jit, code+1)) != 0) {
		char buf[32];
		sprintf(buf, "%x", err);
		commit_code(jit, code, 0);
		luaL_error(L, "dasm_encode error %s", buf);
	}

	commit_code(jit, code, codesz);
	return (CFunction) (code+1);
}

typedef uint8_t jump_t[JUMP_SIZE];

int get_extern(JIT* jit, uint8_t* addr, int idx, int type)
{
	Page* page = jit->pages[jit->pagenum-1];
	jump_t* jumps = (jump_t*) (page+1);
	JIT_head* h = (JIT_head*) ((uint8_t*) page + page->off);
	uint8_t* jmp;
	ptrdiff_t off;

	if (idx == jit->function_extern) {
	   jmp = h->jump;
	} else {
	   jmp = jumps[idx];
	}

	/* compensate for room taken up for the offset so that we can work rip
	 * relative */
	addr += BRANCH_OFF;

	/* see if we can fit the offset in the branch displacement, if not use the
	 * jump instruction */
	off = *(uint8_t**) jmp - addr;

	if (MIN_BRANCH <= off && off <= MAX_BRANCH) {
		return (int32_t) off;
	} else {
		return (int32_t)(jmp + sizeof(uint8_t*) - addr);
	}
}

static void* reserve_code(JIT* jit, lua_State* L, size_t sz)
{
	size_t off = (jit->pagenum > 0) ? jit->pages[jit->pagenum-1]->off : 0;
	size_t size = (jit->pagenum > 0) ? jit->pages[jit->pagenum-1]->size : 0;

	Page * page;
	if (off + sz >= size) {
		int i;
		uint8_t* pdata;
		CFunction func;

		/* need to create a new page */
		jit->pages = (Page**) realloc(jit->pages, (++jit->pagenum) * sizeof(jit->pages[0]));

		size = ALIGN_UP(sz + LINKTABLE_MAX_SIZE + sizeof(Page), jit->align_page_size);

		page = (Page*) AllocPage(size);
		jit->pages[jit->pagenum-1] = page;
		pdata = (uint8_t*) page;
		page->size = size;
		page->off = sizeof(Page);

		lua_newtable(L);

#define ADDFUNC(DLL, NAME) \
		lua_pushliteral(L, #NAME); \
		func = DLL ? (CFunction) GetProcAddressA(DLL, #NAME) : NULL; \
		func = func ? func : (CFunction) &NAME; \
		lua_pushcfunction(L, (lua_CFunction) func); \
		lua_rawset(L, -3)

		ADDFUNC(NULL, check_double);
		ADDFUNC(NULL, check_float);
		ADDFUNC(NULL, check_uint64);
		ADDFUNC(NULL, check_int64);
		ADDFUNC(NULL, check_int32);
		ADDFUNC(NULL, check_uint32);
		ADDFUNC(NULL, check_uintptr);
		ADDFUNC(NULL, check_enum);
		ADDFUNC(NULL, check_typed_pointer);
		ADDFUNC(NULL, check_typed_cfunction);
		ADDFUNC(NULL, check_complex_double);
		ADDFUNC(NULL, check_complex_float);
		ADDFUNC(NULL, unpack_varargs_stack);
		ADDFUNC(NULL, unpack_varargs_stack_skip);
		ADDFUNC(NULL, unpack_varargs_reg);
		ADDFUNC(NULL, unpack_varargs_float);
		ADDFUNC(NULL, unpack_varargs_int);
		ADDFUNC(NULL, push_cdata);
		ADDFUNC(NULL, push_int);
		ADDFUNC(NULL, push_uint);
		ADDFUNC(NULL, lua_pushinteger);
		ADDFUNC(NULL, push_float);
		ADDFUNC(jit->kernel32_dll, SetLastError);
		ADDFUNC(jit->kernel32_dll, GetLastError);
		ADDFUNC(jit->lua_dll, luaL_error);
		ADDFUNC(jit->lua_dll, lua_pushnumber);
		ADDFUNC(jit->lua_dll, lua_pushboolean);
		ADDFUNC(jit->lua_dll, lua_gettop);
		ADDFUNC(jit->lua_dll, lua_rawgeti);
		ADDFUNC(jit->lua_dll, lua_pushnil);
		ADDFUNC(jit->lua_dll, lua_callk);
		ADDFUNC(jit->lua_dll, lua_settop);
		ADDFUNC(jit->lua_dll, lua_remove);
#undef ADDFUNC

		for (i = 0; extnames[i] != NULL; i++) {

			if (strcmp(extnames[i], "FUNCTION") == 0) {
				shred(pdata + page->off, 0, JUMP_SIZE);
				jit->function_extern = i;

			} else {
				lua_getfield(L, -1, extnames[i]);
				func = (CFunction) lua_tocfunction(L, -1);

				if (func == NULL) {
					luaL_error(L, "internal error: missing link for %s", extnames[i]);
				}

				compile_extern_jump(jit, L, func, pdata + page->off);
				lua_pop(L, 1);
			}

			page->off += JUMP_SIZE;
		}

		page->freed = page->off;
		lua_pop(L, 1);

	} else {
		page = jit->pages[jit->pagenum-1];
		EnableWrite(page, page->size);
	}

	return (uint8_t*) page + page->off;
}

static void commit_code(JIT* jit, void* code, size_t sz)
{
	Page* page = jit->pages[jit->pagenum-1];
	page->off += sz;
	EnableExecute(page, page->size);
	{
#if 0
		FILE* out = fopen("\\Hard Disk\\out.bin", "wb");
		fwrite(page, page->off, 1, out);
		fclose(out);
#endif
	}
}

/* push_func_ref pushes a copy of the upval table embedded in the compiled
 * function func.
 */
void push_func_ref(lua_State* L, CFunction func)
{
	JIT_head* h = ((JIT_head*) func) - 1;
	lua_rawgeti(L, LUA_REGISTRYINDEX, h->ref);
}

void free_code(JIT* jit, lua_State* L, CFunction func)
{
	size_t i;
	JIT_head* h = ((JIT_head*) func) - 1;
	for (i = 0; i < jit->pagenum; i++) {
		Page* p = jit->pages[i];

		if ((uint8_t*) h < (uint8_t*) p || (uint8_t*) p + p->size <= (uint8_t*) h) {
			continue;
		}

		luaL_unref(L, LUA_REGISTRYINDEX, h->ref);

		EnableWrite(p, p->size);
		p->freed += h->size;

		shred(h, 0, h->size);

		if (p->freed < p->off) {
			EnableExecute(p, p->size);
			return;
		}

		FreePage(p, p->size);
		memmove(&jit->pages[i], &jit->pages[i+1], (jit->pagenum - (i+1)) * sizeof(jit->pages[0]));
		jit->pagenum--;
		return;
	}

	assert(!"couldn't find func in the jit pages");
}
