Me picking up the facebook-archive luaffifb project.  Of all the lua-ffi projects, this one seemed furthest along.

# Changes I've made since forking it:

- `ffi.null` as well as `ffi.NULL`
- ctype objects can now use their metatables' `__index` just like in vanilla LuaJIT.

# Changes still to make:

- `print(ffi.cast('void*', ffi.cast('vec3f_t*', 0)+1))` should show `0xc`, but instead it shows `vec3f(1,1,1)` ... an error in luaffifb with its cdata of pointers' metamethods, esp +, is treating the pointer as its data when it should be doing pointer-arithmetic.
- why do I think `tonumber(ffi.new('int64_t', 0))` is serializing first and then parsing the string to a number?  Is there a better way to convert int64 boxed type to lua integer ...
- indexing fields that aren't there should throw exceptions.  I hate it, but I'm staying true to LuaJIT.  or maybe I shouldn't, idk...


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
this upgrade to Lua 5.2 or use boxed numbers (uint64_t and uintptr_t).
- All bitfields are treated as unsigned (does anyone even use signed
  bitfields?). Note that "int s:8" is unsigned on unix x86/x64, but signed on
windows.


How it works
------------
Types are represented by a struct ctype structure and an associated user value
table. The table is shared between all related types for structs, unions, and
functions. It's members have the types of struct members, function argument
types, etc. The struct ctype structure then contains the modifications from
the base type (eg number of pointers, array size, etc).

Types are pushed into lua as a userdata containing the struct ctype with a
user value (or fenv in 5.1) set to the shared type table.

Boxed cdata types are pushed into lua as a userdata containing the struct
cdata structure (which contains the struct ctype of the data as its header)
followed by the boxed data.

The functions in `ffi.C` provide the `cdata` and `ctype` metatables and ffi.*
functions which manipulate these two types.

C functions (and function pointers) are pushed into lua as a lua c function
with the function pointer cdata as the first upvalue. The actual code is JITed
using dynasm (see call_x86.dasc). The JITed code does the following in order:

1. Calls the needed unpack functions in `ffi.C` placing each argument on the HW stack
2. Updates `errno`
3. Performs the C call
4. Retrieves `errno`
5. Pushes the result back into lua from the HW register or stack
