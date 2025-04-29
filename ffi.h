/* vim: ts=4 sw=4 sts=4 et tw=78
 * Portions copyright (c) 2015-present, Facebook, Inc. All rights reserved.
 * Portions copyright (c) 2011 James R. McKaskill.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lua.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#endif

#include "ffi_complex.h"

#define HAVE_LONG_DOUBLE


#if defined LUA_FFI_BUILD_AS_DLL
# define EXPORT __declspec(dllexport)
#elif defined __GNUC__
# define EXPORT __attribute__((visibility("default")))
#else
# define EXPORT
#endif

#ifdef __cplusplus
# define EXTERN_C extern "C"
#else
# define EXTERN_C extern
#endif

EXTERN_C EXPORT int luaopen_ffi(lua_State* L);


// architectures
#if defined _WIN32 && defined UNDER_CE
# define OS_CE
#elif defined _WIN32
# define OS_WIN
#elif defined __APPLE__ && defined __MACH__
# define OS_OSX
#elif defined __linux__
# define OS_LINUX
#elif defined __EMSCRIPTEN__ || defined __WASM__
# define OS_BROWSER
#elif defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
# define OS_BSD
#elif defined unix || defined __unix__ || defined __unix || defined _POSIX_VERSION || defined _XOPEN_VERSION
# define OS_POSIX
#endif

// architecture
#if defined __i386__ || defined _M_IX86
# define ARCH_X86
#elif defined __amd64__ || defined _M_X64
# define ARCH_X64
#elif defined __arm__ || defined __ARM__ || defined ARM || defined __ARM || defined __arm || defined __aarch64__
# define ARCH_ARM
#elif defined __powerpc64__
# define ARCH_PPC64
#elif defined __EMSCRIPTEN__ || defined __WASM__
# define ARCH_WASM
#else
# error unknown arch
#endif


// enable this to skip out on JIT-based calling and use libffi instead
//#define CALL_WITH_LIBFFI


#ifdef _WIN32

#   ifdef UNDER_CE
		inline void* DoLoadLibraryA(const char* name) {
		  wchar_t buf[MAX_PATH];
		  int sz = MultiByteToWideChar(CP_UTF8, 0, name, -1, buf, 512);
		  if (sz > 0) {
			buf[sz] = 0;
			return LoadLibraryW(buf);
		  } else {
			return NULL;
		  }
		}
#	   define LoadLibraryA DoLoadLibraryA
#   else
#	   define GetProcAddressA GetProcAddress
#   endif

#   define LIB_FORMAT_1 "%s.dll"
#   define AllocPage(size) VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)
#   define FreePage(data, size) VirtualFree(data, 0, MEM_RELEASE)
#   define EnableExecute(data, size) do {DWORD old; VirtualProtect(data, size, PAGE_EXECUTE, &old); FlushInstructionCache(GetCurrentProcess(), data, size);} while (0)
#   define EnableWrite(data, size) do {DWORD old; VirtualProtect(data, size, PAGE_READWRITE, &old);} while (0)

#else
#ifdef OS_OSX
#   define LIB_FORMAT_1 "%s.dylib"
#   define LIB_FORMAT_2 "lib%s.dylib"
#else
#   define LIB_FORMAT_1 "%s.so"
#   define LIB_FORMAT_2 "lib%s.so"
#endif
#   define LoadLibraryA(name) dlopen(name, RTLD_LAZY | RTLD_GLOBAL)
#   define GetProcAddressA(lib, name) dlsym(lib, name)
#   define AllocPage(size) mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)
#   define FreePage(data, size) munmap(data, size)
#   define EnableExecute(data, size) mprotect(data, size, PROT_READ|PROT_EXEC)
#   define EnableWrite(data, size) mprotect(data, size, PROT_READ|PROT_WRITE)
#endif

#if defined ARCH_X86 || defined ARCH_X64
#define ALLOW_MISALIGNED_ACCESS
#endif

#define ALIGN_DOWN(PTR, MASK) \
  (((uintptr_t) (PTR)) & (~ ((uintptr_t) (MASK)) ))
#define ALIGN_UP(PTR, MASK) \
  (( ((uintptr_t) (PTR)) + ((uintptr_t) (MASK)) ) & (~ ((uintptr_t) (MASK)) ))

// CData/CType

#define PTR_ALIGN_MASK (sizeof(void*) - 1)
#define FUNCTION_ALIGN_MASK (sizeof(void (*)()) - 1)
#define DEFAULT_ALIGN_MASK 7

#ifdef OS_OSX
// TODO: figure out why the alignof trick doesn't work on OS X
#define ALIGNED_DEFAULT 7
#elif defined __GNUC__
#define ALIGNED_DEFAULT (__alignof__(void* __attribute__((aligned))) - 1)
#else
#define ALIGNED_DEFAULT PTR_ALIGN_MASK
#endif

extern int jit_key;
extern int ctype_mt_key;
extern int cdata_mt_key;
extern int cmodule_mt_key;
extern int callback_mt_key;
extern int constants_key;
extern int types_key;
extern int gc_key;
extern int callbacks_key;
extern int functions_key;
extern int abi_key;
extern int next_unnamed_key;
extern int niluv_key;
extern int asmname_key;

int equalsRegistry(lua_State* L, int idx, void * key);
void pushRegistry(lua_State* L, void * key);
void setRegistry(lua_State* L, void * key);

/* both ctype and cdata are stored as userdatas
 *
 * usr value is a table shared between the related subtypes which has:
 * name -> member ctype (for structs and unions)
 * +ves -> member ctype - in memory order (for structs)
 * +ves -> argument ctype (for function prototypes)
 * 0 -> return ctype (for function prototypes)
 * light userdata -> misc
 */

enum {
	C_CALL,
	STD_CALL,
	FAST_CALL,
};

enum {
	INVALID_TYPE,
	VOID_TYPE,
	FLOAT_TYPE,
	DOUBLE_TYPE,
	LONG_DOUBLE_TYPE,
	COMPLEX_FLOAT_TYPE,
	COMPLEX_DOUBLE_TYPE,
	COMPLEX_LONG_DOUBLE_TYPE,
	BOOL_TYPE,
	INT8_TYPE,
	INT16_TYPE,
	INT32_TYPE,
	INT64_TYPE,
	INTPTR_TYPE,
	ENUM_TYPE,
	UNION_TYPE,
	STRUCT_TYPE,
	FUNCTION_TYPE,
	FUNCTION_PTR_TYPE,	// why separate FUNCTION_PTR_TYPE instead of just FUNCTION_TYPE with pointers set?
};

#define IS_CHAR_UNSIGNED (((char) -1) > 0)
#define IS_COMPLEX(type) ((type) == COMPLEX_FLOAT_TYPE || (type) == COMPLEX_DOUBLE_TYPE)

#define ALIGNOF(S) ((int) ((char*) &S.v - (char*) &S - 1))

#include "types.h"	// CFunction

int push_user_mt(lua_State* L, int ct_usr, const CType* ct);
int x86_return_size(lua_State* L, int usr, const CType* ct);

// WARNING: assembly needs to be updated for prototype changes of these functions
int check_bool(lua_State* L, int idx);
double check_double(lua_State* L, int idx);
double check_complex_imag(lua_State* L, int idx);
float check_float(lua_State* L, int idx);
uint64_t check_uint64(lua_State* L, int idx);
int64_t check_int64(lua_State* L, int idx);
int32_t check_int32(lua_State* L, int idx);
uint32_t check_uint32(lua_State* L, int idx);
uintptr_t check_uintptr(lua_State* L, int idx);
int32_t check_enum(lua_State* L, int idx, int to_usr, const CType* tt);

// these two will always push a value so that we can create structs/functions on the fly
void* check_typed_pointer(lua_State* L, int idx, int to_usr, const CType* tt);
CFunction check_typed_cfunction(lua_State* L, int idx, int to_usr, const CType* tt);
complex_double check_complex_double(lua_State* L, int idx);
complex_float check_complex_float(lua_State* L, int idx);

void unpack_varargs_stack(lua_State* L, int first, int last, char* to);
void unpack_varargs_reg(lua_State* L, int first, int last, char* to);

void unpack_varargs_stack_skip(lua_State* L, int first, int last, int ints_to_skip, int floats_to_skip, char* to);
void unpack_varargs_float(lua_State* L, int first, int last, int max, char* to);
void unpack_varargs_int(lua_State* L, int first, int last, int max, char* to);
void print_type(lua_State* L, const CType* ct);
