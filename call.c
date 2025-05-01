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
#include "parser.h"	//push_type_name
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
#ifdef __cplusplus
extern "C" {	// because linking errors with the &ffi_type_* externs
#endif
#include <ffi.h>
#ifdef __cplusplus
}
#endif


#ifndef FFI_TARGET_HAS_COMPLEX_TYPE
#error FFI_TARGET_HAS_COMPLEX_TYPE should be enabled in your libffi ffi.h
#endif
#ifndef HAVE_COMPLEX
#error HAVE_COMPLEX should be enabled in your luaffifb ffi_complex.h
#endif




// I would use ffi_raw but it doesn't have double, so here I'm making my own ...
typedef union {
#if defined(__wasm__)	// I want to use the larger of intptr_t and int64_t ... for now here is this ...
	int64_t intptrValue;
	uint64_t uintptrValue;
#else
static_assert(sizeof(intptr_t) >= sizeof(int64_t), "CallValue intptr_t can't handle int64_t fair warning");
	intptr_t intptrValue;
	uintptr_t uintptrValue;
#endif
	float floatValue;
	double doubleValue;
	complex_float complex_floatValue;
	complex_double complex_doubleValue;
	char data[sizeof(complex_double)];
	void* ptr;
} CallValue;
static_assert(sizeof(CallValue) >= sizeof(ffi_raw), "CallValue can't be used to return ffi_call results");
static_assert(sizeof(CallValue) >= sizeof(int64_t), "CallValue can't be used for int64_t results");

typedef struct CallInfo {
	ffi_cif cif;
	CFunction func;
	int nargs;
	ffi_type ** argTypes;
	ffi_type * retType;
	void ** valuePtrs;	//allocated upon creation, size nargs, points into valueData
	CallValue * valueData;
} CallInfo;

void compile_globals(JIT* jit, lua_State* L) {}

static inline ffi_type * getFFITypeForCType(
	lua_State * L,			// only used for luaL_error
	CType const * ctype
) {
	if (ctype->pointers || ctype->is_reference) {
		return &ffi_type_pointer;
	}
	switch (ctype->type) {
	case VOID_TYPE:
		return &ffi_type_void;
	case BOOL_TYPE:
		return &ffi_type_sint8;
	case INT8_TYPE:
		return ctype->is_unsigned ? &ffi_type_uint8 : &ffi_type_sint8;
	case INT16_TYPE:
		return ctype->is_unsigned ? &ffi_type_uint16 : &ffi_type_sint16;
	case INT32_TYPE:
		return ctype->is_unsigned ? &ffi_type_uint32 : &ffi_type_sint32;
	case FLOAT_TYPE:
		return &ffi_type_float;
	case DOUBLE_TYPE:
		return &ffi_type_double;
	case ENUM_TYPE:
		return ctype->is_unsigned ? &ffi_type_uint32 : &ffi_type_sint32;
	case INT64_TYPE:
		return ctype->is_unsigned ? &ffi_type_uint64 : &ffi_type_sint64;
	case COMPLEX_FLOAT_TYPE:
		return &ffi_type_complex_float;
	case COMPLEX_DOUBLE_TYPE:
		return &ffi_type_complex_double;
	case FUNCTION_PTR_TYPE:
		return &ffi_type_pointer;
	default:
		luaL_error(L, "NYI: call return type");
		return NULL;
	}
}


// use debugging?
//#define DEBUG_LOG
#if defined(DEBUG_LOG)
#define DEBUGPRINT(...) printf(__VA_ARGS__)
#else
#define DEBUGPRINT(...)
#endif

/*
ok i've completely lost track of what is what ...
upvalues:
#1: ctypeUserValue = the function's CType's uservalue[1] ... whatever that is
#2: CallInfo userdata

NOTICE the original C function is both in CallInfo->func and in the original userdata<CData + CFunction>
... however the original userdata<CData> isn't visible from within this function.
*/
int callLuaToCWithLibFFI(
	lua_State *L
) {						// stack: closure_func, args...
DEBUGPRINT("callLuaToCWithLibFFI BEGIN, top=%d\n", lua_gettop(L));
	// CData userdata of the function is in upvalue[1]
	// Does anyone ever use this, both here and in call_*.h ?
	// In both our cases a lua_CFunction is returned
	// Looks like call_*.h for compile_function_callback() cdata is actually returned.
	// Maybe I can switch call_*.h compile_function() to do the same in order to get the casting C function<->cdata bug fixed?

	// function CType userdata uservalue[1]
	int ctypeUserValueLoc = lua_upvalueindex(1);

	// get closure arg #3 as the CalInfo that holds the ffi_cif
	CallInfo * callInfo = (CallInfo*)lua_touserdata(L, lua_upvalueindex(2));
DEBUGPRINT("...callInfo %p\n", callInfo);

	// translate all the Lua args into FFI args
	assert(callInfo->cif.nargs == callInfo->nargs);
	for (int i = 1; i <= callInfo->nargs; ++i) {
		lua_rawgeti(L, ctypeUserValueLoc, i);		// stack: closure_func, args..., arg[i]'s CType's userdata = closure_func's upvalue[2]'s [i]
		const CType * argCType = (const CType*) lua_touserdata(L, -1);

		CallValue * argValue = &callInfo->valueData[i-1];
		assert(callInfo->valuePtrs[i-1] == argValue);

		lua_getuservalue(L, -1);			// stack: closure_func, args..., args[i]'s CType's userdata, arg[i]'s CType's userdata's uservalue[1]
		int argCTypeUserValueLoc = lua_gettop(L);	// not retarded at all.

#if defined(DEBUG_LOG)
push_type_name(L, -1, argCType);
DEBUGPRINT("...setting arg #%d @%p of luaffi-type=%s libffi-type-ptr=%p\n", i, argValue, lua_tostring(L, -1), callInfo->cif.arg_types[i-1]);
lua_pop(L, 1);			// pop typename
#endif

DEBUGPRINT("BEGIN READ ARG VALUE\n");
		if (argCType->pointers || argCType->is_reference) {
DEBUGPRINT("...pointer or reference...\n");
			argValue->ptr = (void*)cast_uint64(L, i, 1);
		} else {
			// TODO don't just reuse uint64 for everything, what if the endian-ness is opposite x64?
			switch (argCType->type) {
			case FUNCTION_PTR_TYPE:
DEBUGPRINT("...FUNCTION_PTR_TYPE...\n");
				argValue->ptr = (void*)cast_uint64(L, i, 1);
DEBUGPRINT("...%p\n", argValue->ptr);
				break;
			case ENUM_TYPE:
DEBUGPRINT("...ENUM_TYPE...\n");
				argValue->intptrValue = check_enum(L, i, argCTypeUserValueLoc, argCType);	//not retarded at all.
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				break;
			case BOOL_TYPE:
DEBUGPRINT("...BOOL_TYPE...\n");
				argValue->intptrValue = (cast_int64(L, i, 1) != 0);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				break;
			case INT8_TYPE:
DEBUGPRINT("...INT8_TYPE...\n");
				if (argCType->is_unsigned) {
					argValue->uintptrValue = cast_uint64(L, i, 1);
DEBUGPRINT("...%lu\n", argValue->uintptrValue);
				} else {
					argValue->intptrValue = cast_int64(L, i, 1);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				}
				break;
			case INT16_TYPE:
DEBUGPRINT("...INT16_TYPE...\n");
				if (argCType->is_unsigned) {
					argValue->uintptrValue = cast_uint64(L, i, 1);
DEBUGPRINT("...%lu\n", argValue->uintptrValue);
				} else {
					argValue->intptrValue = cast_int64(L, i, 1);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				}
				break;
			case INT32_TYPE:
DEBUGPRINT("...INT32_TYPE...\n");
				if (argCType->is_unsigned) {
					argValue->uintptrValue = cast_uint64(L, i, 1);
DEBUGPRINT("...%lu\n", argValue->uintptrValue);
				} else {
					argValue->intptrValue = cast_int64(L, i, 1);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				}
				break;
			case INT64_TYPE:
DEBUGPRINT("...INT64_TYPE...\n");
				if (argCType->is_unsigned) {
					argValue->uintptrValue = cast_uint64(L, i, 1);
DEBUGPRINT("...%lu\n", argValue->uintptrValue);
				} else {
					argValue->intptrValue = cast_int64(L, i, 1);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				}
				break;
			case INTPTR_TYPE:
DEBUGPRINT("...INTPTR_TYPE...\n");
				if (argCType->is_unsigned) {
					argValue->uintptrValue = cast_uint64(L, i, 1);
DEBUGPRINT("...%lu\n", argValue->uintptrValue);
				} else {
					argValue->intptrValue = cast_int64(L, i, 1);
DEBUGPRINT("...%ld\n", argValue->intptrValue);
				}
				break;
			case FLOAT_TYPE:
DEBUGPRINT("...FLOAT_TYPE...\n");
				argValue->floatValue = check_float(L, i);
DEBUGPRINT("...%f\n", argValue->floatValue);
				break;
			case DOUBLE_TYPE:
DEBUGPRINT("...DOUBLE_TYPE...\n");
				argValue->doubleValue = check_double(L, i);
DEBUGPRINT("...%f\n", argValue->doubleValue);
				break;
			case COMPLEX_FLOAT_TYPE:	// TODO FIXME
DEBUGPRINT("...COMPLEX_FLOAT_TYPE...\n");
				argValue->complex_floatValue = check_complex_float(L, i);
DEBUGPRINT("...%f %f\n", crealf(argValue->complex_floatValue), cimagf(argValue->complex_floatValue));
				break;
			case COMPLEX_DOUBLE_TYPE:	// TODO FIXME
DEBUGPRINT("...COMPLEX_DOUBLE_TYPE...\n");
				argValue->complex_doubleValue = check_complex_double(L, i);
DEBUGPRINT("...%f %f\n", creal(argValue->complex_doubleValue), cimag(argValue->complex_doubleValue));
				break;
			default:
				luaL_error(L, "NYI: call type");
			}
		}
DEBUGPRINT("END READ ARG VALUE\n");

		lua_pop(L, 1);			// stack: closure_func, args..., args[i]'s CType's userdata
		lua_pop(L, 1);			// stack: closure_func, args...
	}

	//what about when sizeof(int64) > sizeof(intptr),
	//or what about when sizeof(double) > sizeof(intptr) ?
	//do they require return pointer to point to allocated space?
	// I bet I need to allocate this up front, but only for certain types ... that are larger than intptr ...
DEBUGPRINT("calling func=%p\n", callInfo->func);
	// do the call
	CallValue ret;
	memset(&ret, 0, sizeof(ret));

#if defined(DEBUG_LOG)
DEBUGPRINT("...with %d args\n", callInfo->cif.nargs);
for (int i = 0; i < callInfo->nargs; ++i) {
	printf("... arg[%d] ptr=%p value=", i, callInfo->valuePtrs[i]);
	for (int j = 0; j < sizeof(CallValue); ++j) {
		printf(" %02x", ((uint8_t*)callInfo->valuePtrs[i])[j]);
	}
	printf("\n");
}
#endif

	ffi_call(&callInfo->cif, FFI_FN(callInfo->func), &ret, callInfo->valuePtrs);

	int nresult = 1;

	// TODO translate the Lua result to C result
	{
		lua_rawgeti(L, ctypeUserValueLoc, 0);		// stack: closure_func, args..., return type's CType's userdata = closure_func's upvalue[2]'s [0]
		CType const * retCType = (CType const *)lua_touserdata(L, -1);

		// So when creating CData, I'm supposed to get the CType's uservalue1 and forward that on to the CData's uservalue1, right?
		// I think I see that going on in `do_new` ...
		lua_getuservalue(L, -1);		// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1]
		int retCTypeUserValueLoc = lua_gettop(L);

		push_type_name(L, -1, retCType);
DEBUGPRINT("...ret luaffi-type=%d name=%s libffi-type-ptr=%p\n", retCType->type, lua_tostring(L, -1), callInfo->cif.rtype);
		lua_pop(L, 1);

		if (retCType->pointers || retCType->is_reference) {
			// the function returned a pointer ...
			// now we wrap it in CData

			// TODO WHAT ARE THE MAGIC USERVALUES THAT GO WITH THE CDATA?!?!?!?!? THEY AREN'T DOCUMENTED ANYWHERE I LOOK AND THEY ARE ARBITRARY DEPENDING ON THE UNDERLYING CDATA / CTYPE !!!!!
			void ** ptr = (void **)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
DEBUGPRINT("...pointer %p\n", ret.ptr);
			ptr[0] = ret.ptr;

		} else if (retCType->type == VOID_TYPE) {
			nresult = 0;
		} else {
			switch (retCType->type) {
			case FUNCTION_PTR_TYPE:
				{
					CFunction * p = (CFunction *)push_cdata(L, retCTypeUserValueLoc, retCType);		// stack: typedesc, args..., typedesc's CType's uservalue[1], userdata of CData of CType ct
					p[0] = callInfo->func;
DEBUGPRINT("returning func ptr %p into container %p\n", p[0], p);
				}
				break;
			case BOOL_TYPE:
				lua_pushboolean(L, ret.uintptrValue);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return boolean
				break;

			case ENUM_TYPE:
			case INT8_TYPE:
			case INT16_TYPE:
			case INT32_TYPE:
				if (retCType->is_unsigned) {
					lua_pushnumber(L, (lua_Number)ret.uintptrValue);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				} else {
					lua_pushnumber(L, (lua_Number)ret.intptrValue);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				}
				break;

			case INT64_TYPE:
				// uhm, does it always allocate 8 bytes?
				if (retCType->is_unsigned) {
					uint64_t * ptr = (uint64_t *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.uintptrValue;
				} else {
					int64_t * ptr = (int64_t *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.intptrValue;
				}
				break;

			// TODO test intptr_t size, and use boxed type vs lua number type?
			case INTPTR_TYPE:
				if (retCType->is_unsigned) {
					uintptr_t * ptr = (uintptr_t *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.uintptrValue;
				} else {
					intptr_t * ptr = (intptr_t *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.intptrValue;
				}
				break;

			case FLOAT_TYPE:
				lua_pushnumber(L, ret.floatValue);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				break;
			case DOUBLE_TYPE:
				lua_pushnumber(L, ret.doubleValue);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return number
				break;
			case COMPLEX_FLOAT_TYPE:
				{
					complex_float * ptr = (complex_float *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					// TODO pointer-to-result, this will overflow and write oob
					ptr[0] = ret.complex_floatValue;
				}
				break;
			case COMPLEX_DOUBLE_TYPE:
				{
					complex_double * ptr = (complex_double *)push_cdata(L, -1, retCType);	// stack: closure_func, args..., return type's CType's userdata, return type's CType's userdata's uservalue[1], return CData's uservalue
					ptr[0] = ret.complex_doubleValue;
				}
				break;
			default:
				luaL_error(L, "NYI: call return type");
			}
		}
	}

DEBUGPRINT("callLuaToCWithLibFFI DONE, top=%d returning %d\n\n", lua_gettop(L), nresult);
	return nresult;
}

/*
funcCTypeUserValueLoc = index of the function's ctype's uservalue[1]
*/
void compile_function(
	lua_State * L,
	CFunction func,
	int funcCTypeUserValueLoc,					// userdata of CType's uservalue[1] ... what are these used for again?  "usr" for the uservalue doesn't lend much of an explanation ...
	const CType * ctype
) {								// stack: ...
DEBUGPRINT("compile_function() BEGIN func=%p\n", func);

	int top = lua_gettop(L);
	funcCTypeUserValueLoc = lua_absindex(L, funcCTypeUserValueLoc);

	// TODO varag, because libffi handles it.
	if (ctype->calling_convention != C_CALL && ctype->has_var_arg) {
		luaL_error(L, "vararg is only allowed with the c calling convention");
	}

	/*
	push_cdata() sets cdata's uservalue[1] to CType's uservalue[1]
	*/
	CFunction * cdata = push_cdata(L, funcCTypeUserValueLoc, ctype);	// stack: ..., cdata = CData userdata for type ctype, which should be a function
	cdata[0] = func;

	// fill out types
	size_t nargs = lua_rawlen(L, funcCTypeUserValueLoc);

	lua_pushvalue(L, funcCTypeUserValueLoc);					// stack: ..., cdata, ctypeUserVal = stack[funcCTypeUserValueLoc]

	// make one giant allocation so I don't have to worry about my own __gc to free up stuff, because there seems to be exit race conditions where CallInfo's get freed and then their function called,which has a bad CallInfo ...
	size_t callInfoBufSize =
		sizeof(CallInfo)
		+ sizeof(ffi_type*) * nargs	//  argType
		+ sizeof(CallValue) * nargs	// valueData
		+ sizeof(void*) * nargs;	// valuePtrs
	CallInfo * callInfo = (CallInfo*)lua_newuserdata(L, callInfoBufSize);	// stack: ..., cdata, ctypeUserVal, callInfo = userdata of CallInfo
DEBUGPRINT("...callInfo %p\n", callInfo);
	memset(callInfo, 0, callInfoBufSize);
	{
		uint8_t * p = (uint8_t *)callInfo + sizeof(CallInfo);
		callInfo->func = func;
		callInfo->nargs = nargs;
		callInfo->argTypes = (ffi_type**)p;
		p += nargs * sizeof(ffi_type*);
		callInfo->valueData = (CallValue*)p;
		p += nargs * sizeof(CallValue);
		callInfo->valuePtrs = (void**)p;
		p += nargs * sizeof(void*);
		assert(p == (uint8_t*)callInfo+callInfoBufSize);
	}

	/*
	Does this mean a function's CType userdata's uservalue[1] is a table of:
	[0] = userdata of the CType of the return type
	[i] = userdata of the CType of the i'th arg type, for i>0
	*/
	for (int i = 1; i <= nargs; i++) {
		callInfo->valuePtrs[i-1] = &callInfo->valueData[i-1];
		lua_rawgeti(L, funcCTypeUserValueLoc, i);						// stack: ..., cdata, ctypeUserVal, callInfo, ctypeUserVal[i]
		CType const * argCType = (CType const *)lua_touserdata(L, -1);
		callInfo->argTypes[i-1] = getFFITypeForCType(L, argCType);
#if defined(DEBUG_LOG)
//print_type(L, argCType);	// overly cmoplex and worthless
lua_getuservalue(L, -1);
push_type_name(L, -1, argCType);
DEBUGPRINT("args[%d] setting luaffi-type-name=%s libffi-type-ptr=%p libffi-type=%d\n", i, lua_tostring(L, -1), callInfo->argTypes[i-1], callInfo->argTypes[i-1]->type);
lua_pop(L, 2);	// typename string & arg's ctype's userdata's uservalue
#endif
		lua_pop(L, 1);													// stack: ..., cdata, ctypeUserVal, callInfo
	}

	lua_rawgeti(L, funcCTypeUserValueLoc, 0);							// stack: ..., cdata, ctypeUserVal, callInfo, ctypeUserVal[0]
	CType const * retCType = (CType const *)lua_touserdata(L, -1);
	callInfo->retType = getFFITypeForCType(L, retCType);
#if defined(DEBUG_LOG)
//print_type(L, retCType);
lua_getuservalue(L, -1);
push_type_name(L, -1, retCType);
DEBUGPRINT("return luaffi-type-name=%s libffi-type-ptr=%p libffi-type=%d\n", lua_tostring(L, -1), callInfo->retType, callInfo->retType->type);
lua_pop(L, 2);	// typename string & arg's ctype's userdata's uservalue
#endif
	lua_pop(L, 1);													// stack: ..., cdata, ctypeUserVal, callInfo

	// TODO
	// https://www.chiark.greenend.org.uk/doc/libffi-dev/html/The-Basics.html
	// "If the function being called is variadic (varargs) then ffi_prep_cif_var must be used instead of ffi_prep_cif."
	ffi_status prepResult = ffi_prep_cif(&callInfo->cif, FFI_DEFAULT_ABI, nargs, callInfo->retType, callInfo->argTypes);
	if (prepResult != FFI_OK) {
		luaL_error(L, "ffi_prep_cif failed with %d", prepResult);
	}

	/*
	so when __call on the CData of a CFunction happens it had better match spec
	so what is that spec?
	This is just a lua_CFunction stored in module[key]
	So it just executes like any other function would
	*/

	lua_pushvalue(L, -2);
	lua_pushvalue(L, -2);
	lua_pushcclosure(L, callLuaToCWithLibFFI, 2);	// stack: ..., cdata, ctypeUserVal, callInfo, callLuaToCWithLibFFI;  ... with upvalues of {ctypeUserVal, callInfo}

#if 1
	/*
	So it looks like in the call_x64.h compile_function() does push a CData<CFunction> userdata onto the stack and just toss it,
	because then it pushes the lua_CFunction of the closure onto the stack, 
	and any calls just goes to that lua_CFunction, and that's what we use from then on out.
	Nobody sees the CData again, only the lua_CFunction, and that's why you cannot cast a dlsym'd function to void* or other CData-pointers. (A feature missing that's in original LuaJIT)

	Then there's call_x64.h's compile_callback(), and that does seem to push and leave the CData on the stack.
	And then that CData's call behind-the-scenes uservalue[]'s are specified in the `cdata_call` function in ffi.c
	*/

	lua_remove(L, -2);								// stack: ..., cdata, ctypeUserVal, callLuaToCWithLibFFI ... removed callInfo 
// TODO WHERE TO STORE THIS.
// cdata's uservalue[1] [cdata] , for C function-ptrs this holds the closure function.
// IS ANYTHING ELSE USING THIS?
{// lets check
	lua_pushvalue(L, -3);							// stack: ..., cdata, ctypeUserVal, callLuaToCWithLibFFI, cdata
	lua_rawget(L, -3);								// stack: ..., cdata, ctypeUserVal, callLuaToCWithLibFFI, ctypeUserVal[cdata]
	assert(lua_type(L, -1) == LUA_TNIL || lua_tocfunction(L, -1) == callLuaToCWithLibFFI);
	lua_pop(L, 1);									// stack: ..., cdata, ctypeUserVal, callLuaToCWithLibFFI
}

	lua_pushvalue(L, -3);							// stack: ..., cdata, ctypeUserVal, callLuaToCWithLibFFI, cdata
	lua_insert(L, -2);								// stack: ..., cdata, ctypeUserVal, cdata, callLuaToCWithLibFFI
	lua_rawset(L, -3);								// stack: ..., cdata, ctypeUserVal;  ctypeUserVal[cdata] = callLuaToCWithLibFFI
	lua_pop(L, 1);									// stack: ..., cdata
	assert(lua_gettop(L) == top + 1);
#else	// the old way:
						// stack: ..., cdata, ctypeUserVal, callInfo, callLuaToCWithLibFFI
	lua_insert(L, -4); 	// stack: ..., callLuaToCWithLibFFI, cdata, ctypeUserVal, callInfo
	lua_pop(L, 3); 		// stack: ..., callLuaToCWithLibFFI
#endif
DEBUGPRINT("compile_function() DONE\n\n");
}




/* 
TODO always do this, so we're always returning cdata, which is castable, which doesn't run us into the bug that at present module functions cannot be cast to other ptrs
TODO this is gonna push a CData, so the call will have to be handled in cdata_call
so cdata_call will have to support the 
*) old JIT-based closure
*) the old closures-of-CFunctoins from compile_function() below which I gotta get rid of to get ffi-CFunction-casting to work
*) new CData closures that don't use JIT but do use LibFFI
*) new closures-of-CFunctions in compile_functin() TBD
*/
CFunction compile_callback(
	lua_State* L,
	int luaFuncLoc,
	int funcCTypeUserValueLoc,
	CType const * ct
) {									// stack: ...
	luaL_error(L, "TODO callbacks");

	CFunction * pf = (CFunction*)push_cdata(L, funcCTypeUserValueLoc, ct);
	//pf[0] = callCToLuaWithLibFFI;	// compile function ... which converts the C->Lua args, calls, and converts Lua->C return type.
	return *pf;
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

static CFunction compile(JIT * jit, lua_State * L, CFunction func, int ref)
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
