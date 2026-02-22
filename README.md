I am forking the old luaffifb project to get it working with my [lua-ffi-wasm](https://github.com/thenumbernine/lua-ffi-wasm) project, to run my [luajit opengl sdl framework in browser](https://github.com/thenumbernine/glapp-js).

# Fixes I've made

- CType objects as well as CData objects can now use their metatables' `__index` - just like in vanilla LuaJIT.
- Arithmetic on pointers no longer calls into the metatmethod of the underlying CType - just like in vanilla LuaJIT.
- luaffifb's `tonumber` now handles float, double, complex float, complex double, as well as integer types; and it no longer converts pointers and arrays - just like in vanilla LuaJIT.
- `bool` is no longer serialized as `unsigned bool` - just like in vanilla LuaJIT.
- For `CALL_WITH_LIBFFI` I no longer replace C functions with `lua_CFunction`'s that might run a bit faster but cannot be compared or converted to CData.  The trade off is now there's a separate Lua `__call` per C-function call, but now we do get casting C functions to pointers and operate on them.
- CType objects now have `__eq`
- All the `ffi` library arguments that could be either type or ctype objects now supports the `"$"` arguments.
- `__attribute__((aligned()))` now handles expressions

# Changes I've made

- `ffi.null` as well as `ffi.NULL`
- Added `CALL_WITH_LIBFFI` for non-JIT LibFFI-based calling, especially so this can get working in WASM.  At first I dismissed https://github.com/q66/cffi-lua and https://github.com/zhaojh329/lua-ffi for lacking key features that luaffifb had like bitflags, but now I see the good side of using libffi over hacked in dynasm jit calls, and that is for wasm support.  so I haven't yet finished it but maybe I will look at these for inspiration.

# Changes I plan to make:

- `__attribute__((aligned()))` only affects the first of a list, i.e. `int __attribute__((aligned(16))) a, b;` should align a and b but it only does a.
- Indexing fields that aren't there should throw exceptions.  I hate it, but I'm staying true to LuaJIT.  or maybe I shouldn't, idk...
- CTypes can only be up to 3 pointers deep.  libjpeg breaks this.  Do like luajit and let the CType hold a pointer to the base-CType.  It's starting to look more and more like the pure-lua ffi implementation I made should just be converted over and it'll be more feature-rich than the CType/CData implementation here ... though this implementation seems to have the most superior parser.
- Looks like comparing function-pointers was always breaking the test suite ... better fix that.
- get cdecl/fastcall/stdcall/thiscall working in the libffi-based calling.

# Here's the original readme:
<hr>
<hr>
<hr>

About
-----
This is a library for calling C function and manipulating C types from lua. It
is designed to be interface compatible with the FFI library in LuaJIT (see
http://luajit.org/ext_ffi.html). It can parse C function declarations and
struct definitions that have been directly copied out of C header files and
into lua source as a string.

This is a fork of https://github.com/jmckaskill/luaffi

Source
------
https://github.com/facebook/luaffifb

Platforms
---------
Currently supported:
- Linux x86/x64
- OS X x86/x64

Runs with Lua 5.1, 5.2, and 5.3

Build
-----
In a terminal:

```bash
git clone https://github.com/facebook/luaffifb
cd luaffifb
luarocks make
```

Documentation
-------------
This library is designed to be source compatible with LuaJIT's FFI extension. The documentation at http://luajit.org/ext_ffi.html describes the API and semantics.

Pointer Comparison
------------
Use `ffi.NULL` or `ffi.null` instead of `nil` when checking for `NULL` pointers.
```lua
  ffi.new('void *', 0) == ffi.NULL -- true
```

Known Issues
------------
- Comparing a ctype pointer to `nil` doesn't work the same as in LuaJIT (see above).
  This is unfixable with the current metamethod semantics.
- Constant expressions can't handle non integer intermediate values (eg
  offsetof won't work because it manipulates pointers)
- Not all metamethods work with Lua 5.1 (eg char* + number). This is due to
  the way metamethods are looked up with mixed types in Lua 5.1. If you need
this upgrade to Lua 5.2 or use boxed numbers (`uint64_t` and `uintptr_t`).
- All bitfields are treated as unsigned (does anyone even use signed
  bitfields?). Note that "int s:8" is unsigned on unix x86/x64, but signed on
windows.


How it works
------------
Types are represented by a `CType` structure and an associated user value
table. The table is shared between all related types for structs, unions, and
functions. It's members have the types of struct members, function argument
types, etc. The `CType` structure then contains the modifications from
the base type (eg number of pointers, array size, etc).

Types are pushed into lua as a userdata containing the `CType` with a
user value (or fenv in 5.1) set to the shared type table.

Boxed cdata types are pushed into lua as a userdata containing the struct
cdata structure (which contains the `CType` of the data as its header)
followed by the boxed data.

The functions in `ffi.C` provide the `cdata` and `ctype` metatables and ffi.*
functions which manipulate these two types.

C functions (and function pointers) are pushed into lua as a lua c function
with the function pointer cdata as the first upvalue. The actual code is JITed
using dynasm (see `call_x86.dasc`). The JITed code does the following in order:

1. Calls the needed unpack functions in `ffi.C` placing each argument on the HW stack
2. Updates `errno`
3. Performs the C call
4. Retrieves `errno`
5. Pushes the result back into lua from the HW register or stack
