/* vim: ts=4 sw=4 sts=4 et tw=78
 * Portions copyright (c) 2015-present, Facebook, Inc. All rights reserved.
 * Portions copyright (c) 2011 James R. McKaskill.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ffi.h"
#include "call.h"
#include "ctype.h"	// push_cdata only used by CALL_WITH_LIBFFI

// has DASM_CHECKS in it which sometimes is used by the dynasm/dasc_*.h files included below
// also has prototypes for dasm_init/dasm_free which the CALL_WITH_LIBFFI provides stub functions for
#include "dynasm/dasm_proto.h"

/*
Get the JIT* userdata of registry[&jit_key].
Update the jit->L lua State to the arg passed.
Leaves the stack the same.
*/
JIT* get_jit(lua_State* L) {
	JIT* jit;							// stack: ...
	pushRegistry(L, &jit_key);					// stack: ..., registry[&jit_key]=jit
	jit = (JIT*) lua_touserdata(L, -1);
	jit->L = L;									// update Lua state
	lua_pop(L, 1);								// stack: ...
	return jit;
}


#if defined(CALL_WITH_LIBFFI)

// Put this in a "dynasm/dasm_wasm.h" / "call_wasm.h" to be like the other.
// But really, this is going to be the libffi-based calling mechanism, which will work on any OS/ARCH

// wasm libffi compile_ goes here, not somewhere else, cuz I want to generate a diff patch
#include <ffi.h>

// I would use ffi_raw but it doesn't have double, so here I'm making my own ...
static_assert(sizeof(intptr_t) >= sizeof(int64_t), "CallValue intptr_t can't handle int64_t fair warning");
typedef union {
	intptr_t sint;
	uintptr_t uint;
	float flt;
	double dbl;
	complex_float cflt;
	complex_double cdbl;
	char data[sizeof(complex_double)];
	void* ptr;
} CallValue;

typedef struct CallInfo {
	ffi_cif cif;
	CFunction func;
	int nargs;
	// maybe hardcode max args and don't worry about __gc free()ing these if you're lazy
	void ** valuePtrs;	//allocated upon creation, size nargs, points into valueData
	CallValue * valueData;
} CallInfo;

void compile_globals(JIT* jit, lua_State* L) {}

static inline ffi_type * getFFITypeForCType(
	lua_State * L,			// only used for luaL_error
	CType const * mbr_ct
) {
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
	case BOOL_TYPE: return &ffi_type_sint8;
	case INT8_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint8 : &ffi_type_sint8;
	case INT16_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint16 : &ffi_type_sint16;
	case INT32_TYPE: return mbr_ct->is_unsigned ? &ffi_type_uint32 : &ffi_type_sint32;
	case FLOAT_TYPE: return &ffi_type_float;
	case DOUBLE_TYPE: return &ffi_type_double;
	default:
		luaL_error(L, "NYI: call return type");
		return NULL;
	}
}

/*
ok i've completely lost track of what is what ...
upvalues:
#1: whatever ct_usr is (the first upvalue of cdata_call?)
#2: CallInfo userdata
*/
static int luaffi_libffi_call(
	lua_State *L
) {						// stack: closure_func, args...
printf("luaffi_libffi_call BEGIN, top=%d\n", lua_gettop(L));
	// CData userdata of the function is in upvalue[1]
	// Does anyone ever use this, both here and in call_*.h ?
	// In both our cases a lua_CFunction is returned
	// Looks like call_*.h for compile_function_callback() cdata is actually returned.
	// Maybe I can switch call_*.h compile_function() to do the same in order to get the casting C function<->cdata bug fixed?

	// function CType userdata uservalue[1]
	int ct_usr = lua_upvalueindex(2);

	// get closure arg #3 as the CalInfo that holds the ffi_cif
	CallInfo * callInfo = (CallInfo*)lua_touserdata(L, lua_upvalueindex(3));

	// translate all the Lua args into FFI args
	for (int i = 1; i <= callInfo->nargs; ++i) {
		lua_rawgeti(L, ct_usr, i);		// stack: closure_func, args..., arg[i]'s CType's userdata = closure_func's upvalue[2]'s [i]
		const CType * mbr_ct = (const CType*) lua_touserdata(L, -1);
		lua_pop(L, 1);			// stack: closure_func, args...

printf("setting arg %d\n", i);
		CallValue * argValue = &callInfo->valueData[i-1];
		assert(argValue == callInfo->valuePtrs[i-1]);
		if (mbr_ct->pointers || mbr_ct->is_reference) {
			argValue->ptr = (void*)check_uint64(L, i);
		} else {
			switch (mbr_ct->type) {
			case FUNCTION_PTR_TYPE:
printf("...FUNCTION_PTR_TYPE...\n");
				argValue->ptr = (void*)check_uint64(L, i);
printf("...%p\n", argValue->ptr);
				break;
			case ENUM_TYPE:
printf("...ENUM_TYPE...\n");
				argValue->sint = check_int32(L, i);
printf("...%ld\n", argValue->sint);
				break;
			case BOOL_TYPE:
printf("...BOOL_TYPE...\n");
				argValue->sint = (check_int64(L, i) != 0);
printf("...%ld\n", argValue->sint);
				break;
			case INT8_TYPE:
printf("...INT8_TYPE...\n");
				if (mbr_ct->is_unsigned) {
					argValue->uint = check_uint64(L, i);
printf("...%lu\n", argValue->uint);
				} else {
					argValue->sint = check_int64(L, i);
printf("...%ld\n", argValue->sint);
				}
				break;
			case INT16_TYPE:
printf("...INT16_TYPE...\n");
				if (mbr_ct->is_unsigned) {
					argValue->uint = check_uint64(L, i);
printf("...%lu\n", argValue->uint);
				} else {
					argValue->sint = check_int64(L, i);
printf("...%ld\n", argValue->sint);
				}
				break;
			case INT32_TYPE:
printf("...INT32_TYPE...\n");
				if (mbr_ct->is_unsigned) {
					argValue->uint = check_uint64(L, i);
printf("...%lu\n", argValue->uint);
				} else {
					argValue->sint = check_int64(L, i);
printf("...%ld\n", argValue->sint);
				}
				break;
			case INT64_TYPE:
printf("...INT64_TYPE...\n");
				if (mbr_ct->is_unsigned) {
					argValue->uint = check_uint64(L, i);
printf("...%lu\n", argValue->uint);
				} else {
					argValue->sint = check_int64(L, i);
printf("...%ld\n", argValue->sint);
				}
				break;
			case INTPTR_TYPE:
printf("...INTPTR_TYPE...\n");
				if (mbr_ct->is_unsigned) {
					argValue->uint = check_uint64(L, i);
printf("...%lu\n", argValue->uint);
				} else {
					argValue->sint = check_int64(L, i);
printf("...%ld\n", argValue->sint);
				}
				break;
			case FLOAT_TYPE:
printf("...FLOAT_TYPE...\n");
				argValue->flt = check_double(L, i);
printf("...%f\n", argValue->flt);
				break;
			case DOUBLE_TYPE:
printf("...DOUBLE_TYPE...\n");
				argValue->dbl = check_double(L, i);
printf("...%f\n", argValue->dbl);
				break;
			case COMPLEX_FLOAT_TYPE:	// TODO FIXME
printf("...COMPLEX_FLOAT_TYPE...\n");
				argValue->cflt = check_complex_float(L, i);
printf("...%f %f\n", crealf(argValue->cflt), cimagf(argValue->cflt));
				break;
			case COMPLEX_DOUBLE_TYPE:	// TODO FIXME
printf("...COMPLEX_DOUBLE_TYPE...\n");
				argValue->cdbl = check_complex_double(L, i);
printf("...%f %f\n", creal(argValue->cdbl), cimag(argValue->cdbl));
				break;
			default:
				luaL_error(L, "NYI: call type");
			}
		}
	}

	//what about when sizeof(int64) > sizeof(intptr),
	//or what about when sizeof(double) > sizeof(intptr) ?
	//do they require return pointer to point to allocated space?
	// I bet I need to allocate this up front, but only for certain types ... that are larger than intptr ...
printf("calling func=%p\n", callInfo->func);
	// do the call
	CallValue ret;
	memset(&ret, 0, sizeof(ret));

	ffi_call(&callInfo->cif, FFI_FN(callInfo->func), &ret, callInfo->valuePtrs);

	// TODO translate the Lua result to C result
	{
		lua_rawgeti(L, ct_usr, 0);		// stack: closure_func, args..., return type's CType's userdata = closure_func's upvalue[2]'s [0]
		const CType * mbr_ct = (const CType *)lua_touserdata(L, -1);
printf("...ret luaffi type %d\n", mbr_ct->type);

		// So when creating CData, I'm supposed to get the CType's uservalue1 and forward that on to the CData's uservalue1, right?
		// I think I see that going on in `do_new` ...
		lua_getuservalue(L, -1);		// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1]

		if (mbr_ct->pointers || mbr_ct->is_reference) {
			// the function returned a pointer ...
			// now we wrap it in CData

			// TODO WHAT ARE THE MAGIC USERVALUES THAT GO WITH THE CDATA?!?!?!?!? THEY AREN'T DOCUMENTED ANYWHERE I LOOK AND THEY ARE ARBITRARY DEPENDING ON THE UNDERLYING CDATA / CTYPE !!!!!
			void ** ptr = (void **)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
printf("...pointer %p\n", ret.ptr);
			ptr[0] = ret.ptr;

		} else {
			switch (mbr_ct->type) {
			case VOID_TYPE:
				return 0;
			case FUNCTION_PTR_TYPE:
				luaL_error(L, "TODO FUNCTION_PTR_TYPE %s:%d", __FILE__, __LINE__);
				break;
			
			case BOOL_TYPE:
				lua_pushboolean(L, ret.uint);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return boolean
				break;

			case ENUM_TYPE:
			case INT8_TYPE:
			case INT16_TYPE:
			case INT32_TYPE:
				if (mbr_ct->is_unsigned) {
					lua_pushnumber(L, (lua_Number)ret.uint);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				} else {
					lua_pushnumber(L, (lua_Number)ret.sint);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				}
				break;

			case INT64_TYPE:
				// uhm, does it always allocate 8 bytes?
				if (mbr_ct->is_unsigned) {
					uint64_t * ptr = (uint64_t *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.uint;
				} else {
					int64_t * ptr = (int64_t *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.sint;
				}
				break;

			// TODO test intptr_t size, and use boxed type vs lua number type?
			case INTPTR_TYPE:
				if (mbr_ct->is_unsigned) {
					uintptr_t * ptr = (uintptr_t *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.uint;
				} else {
					intptr_t * ptr = (intptr_t *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.sint;
				}
				break;

			case FLOAT_TYPE:
				lua_pushnumber(L, ret.flt);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				break;
			case DOUBLE_TYPE:
				lua_pushnumber(L, ret.dbl);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				break;
			case COMPLEX_FLOAT_TYPE:
				{
					complex_float * ptr = (complex_float *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					// TODO pointer-to-result, this will overflow and write oob
					ptr[0] = ret.flt;
				}
				break;
			case COMPLEX_DOUBLE_TYPE:
				{
					complex_double * ptr = (complex_double *)push_cdata(L, -1, mbr_ct);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.dbl;
				}
				break;
			default:
				luaL_error(L, "NYI: call return type");
			}
		}
	}

printf("luaffi_libffi_call DONE, top=%d\n", lua_gettop(L));
	return 1;
}

CFunction compile_callback(lua_State* L, int fidx, int ct_usr, const CType* ct) {
	luaL_error(L, "TODO compile_callback");
	return (CFunction)NULL;
}

/*
ct_usr = index of the function's ctype's uservalue 1
*/
void compile_function(
	lua_State * L,
	CFunction func,
	int ct_usr,					// userdata of CType's uservalue 1 ... what are these used for again?  "usr" for the uservalue doesn't lend much of an explanation ...
	const CType * ct
) {								// stack: ...
printf("compile_function() BEGIN func=%p\n", func);
	//int top = lua_gettop(L);
	ct_usr = lua_absindex(L, ct_usr);

	if (ct->calling_convention != C_CALL && ct->has_var_arg) {
		luaL_error(L, "vararg is only allowed with the c calling convention");
	}

	void * p = push_cdata(L, ct_usr, ct);	// stack: ..., p = CData userdata for type ct
	*(CFunction *)p = func;

	// fill out types
	size_t nargs = lua_rawlen(L, ct_usr);
	int const maxArgs = 256;
	ffi_type * argFFITypes[maxArgs] = {NULL};
	if (nargs > maxArgs) {
		luaL_error(L, "function call has too many args: %d > %d\n", nargs, maxArgs);
	}

	/*
	Does this mean a function's CType userdata's uservalue 1 is a table of:
	[0] = userdata of the CType of the return type
	[i] = userdata of the CType of the i'th arg type, for i>0
	*/
	for (int i = 1; i <= nargs; i++) {
		lua_rawgeti(L, ct_usr, i);
		CType const * mbr_ct = (CType const *)lua_touserdata(L, -1);
		argFFITypes[i-1] = getFFITypeForCType(L, mbr_ct);
printf("libffi args[%d] type = %d\n", i, argFFITypes[i-1]->type);
		lua_pop(L, 1);
	}

	lua_rawgeti(L, ct_usr, 0);
	CType const * mbr_ct = (CType const *)lua_touserdata(L, -1);
	ffi_type * retFFIType = getFFITypeForCType(L, mbr_ct);
printf("libffi return type = %d\n", retFFIType->type);
	lua_pop(L, 1);

	lua_pushvalue(L, ct_usr);					// stack: ..., p, stack[ct_usr],

	// push the ffi_cif
	CallInfo * callInfo = (CallInfo*)lua_newuserdata(L, sizeof(CallInfo));	// stack: ..., p, stack[ct_usr], CallInfo
	callInfo->func = func;
	callInfo->nargs = nargs;
	callInfo->valueData = (CallValue*)malloc(sizeof(CallValue) * nargs);
	callInfo->valuePtrs = (void**)malloc(sizeof(void*) * nargs);
	for (int i = 0; i < nargs; ++i) {
		callInfo->valuePtrs[i] = &callInfo->valueData[i];
	}

	// TODO
	// https://www.chiark.greenend.org.uk/doc/libffi-dev/html/The-Basics.html
	// "If the function being called is variadic (varargs) then ffi_prep_cif_var must be used instead of ffi_prep_cif."
	ffi_status prepResult = ffi_prep_cif(&callInfo->cif, FFI_DEFAULT_ABI, nargs, retFFIType, argFFITypes);
	if (prepResult != FFI_OK) {
		luaL_error(L, "ffi_prep_cif failed with %d", prepResult);
	}

	/*
	so when __call on the CData of a CFunction happens it had better match spec
	so what is that spec?
	This is just a lua_CFunction stored in module[key]
	So it just executes like any other function would
	*/

	lua_pushcclosure(L, luaffi_libffi_call, 3);	// stack: ..., luaffi_libffi_call with closure of {p, stack[ct_usr], CallInfo}
printf("compile_function() DONE\n");
}

// stub functions

DASM_FDEF void dasm_init(Dst_DECL, int maxsection) {}
DASM_FDEF void dasm_free(Dst_DECL) {}
DASM_FDEF void dasm_setupglobal(Dst_DECL, void **gl, unsigned int maxgl) {}
DASM_FDEF int dasm_link(Dst_DECL, size_t *szp) { return 0; }	// 0 aka DASM_S_OK

void free_code(JIT* jit, lua_State* L, CFunction func) {}

// will I need this one? it looks important...
void push_func_ref(lua_State* L, CFunction func) {
	luaL_error(L, "TODO push_func_ref");
}


#else	// defined(CALL_WITH_LIBFFI)


// has to be here to define DASM_M_GROW & DASM_M_FREE
// has to have call.h before it in order to define Dst_DECL & Dst_REF
#include "dynasm/dasm_internal.h"

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



#if defined _WIN64
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
	code = (JIT_head *)reserve_code(jit, L, codesz);
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

#endif	// defined(CALL_WITH_LIBFFI)
