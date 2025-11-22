#pragma once

/*
I made this at first just to DAG separate out headers, since at first this project had just one single lump header with internal-forward-declarations and external ones (which ofc is just luaopen_ffi)
Now I'm migrating CType and CData in here.
It'd be nice to have c/h files center around CType/CData separately for organization sake.
Random thought: current programming trends are anti-OOP in the name of saving stupid vtable lookup cycles, and pro-functional in its place "because faster" even if it's not faster.  But OOP teaches people MVC, and ppl without OOP education are lacking MVC, and end up writing all their headers into one giant single blob of a file that isn't organized whatsoever.

WHAT ARE THESE STUPID UNDOCUMENTED USERVALUES ASSOCIATED WITH USERDATA OF CDATA/CTYPE/EVERYTHING ELSE?

	CType:
		complex float / complex double - upon creation, these are given an empty table and it's assigned to uservalue[1]
		everything else - upon creation, no uservalue 1
		functions
			- at the time of the call, usrevalue 1 holds a table with:
				[0] = userdata of CType of the return type
				[i] = userdata of CType of i'th argument



	CData:
		- correct me if I'm wrong but `do_new()` `p = push_cdata`, looks like it is giving the CData the same uservalue[1] as the CType's uservalue[1] that created it.

	call.c libffi lua_CFunctions created by compile_function():
		- This is what I'm adding:
			uservalue[1] = userdata of CData of function (whose uservalue[1] should match what's next ...)
			uservalue[2] = uservalue passed to compile_function()
			uservalue[3] = userdata<CallInfo> holding libffi CIF and buffer for holding args & return data.

	call_*.h closure lua_CFunctions created by compile_function():
		- I guess the closure info is private to the call, and created and read by the compile_function() and by the JIT calling code
		but it looks like it is a CFunction[2] of the original C function and of the JIT-generated function for converting Lua<->C and calling it.
*/

typedef void (*CFunction)();

// Welp here is a gaping weakness to the system ... you can only dereference 3 pointers deep.
// libjpeg already breaks this.
// And how come changing POINTER_BITS breaks things?  How come I bet someone's just using the shift operator with magic numbers somewhere in the code...
#define POINTER_BITS 2
#define POINTER_MAX ((1 << POINTER_BITS) - 1)

/* Note: if adding a new member that is associated with a struct/union
 * definition then it needs to be copied over in ctype.c:set_defined for when
 * we create types based off of the declaration alone.
 *
 * Since this is used as a header for every ctype and cdata, and we create a
 * ton of them on the stack, we try and minimise its size.
 */
typedef struct {
	size_t base_size; /* size of the base type in bytes */

	union {
		/* valid if is_bitfield */
		struct {
			/* size of bitfield in bits */
			unsigned bit_size : 7;
			/* offset within the current byte between 0-63 */
			unsigned bit_offset : 6;
		};
		/* Valid if is_array */
		size_t array_size;
		/* Valid for is_variable_struct or is_variable_array. If
		 * variable_size_known (only used for is_variable_struct) then this is
		 * the total increment otherwise this is the per element increment.
		 */
		size_t variable_increment;
	};
	size_t offset;
	unsigned align_mask : 4; /* as (align bytes - 1) eg 7 gives 8 byte alignment */
	unsigned pointers : POINTER_BITS; /* number of dereferences to get to the base type including +1 for arrays */
	unsigned const_mask : POINTER_MAX + 1; /* const pointer mask, LSB is current pointer, +1 for the whether the base type is const */
	unsigned type : 5; /* value given by type enum above */
	unsigned is_reference : 1;
	unsigned is_array : 1;
	unsigned is_defined : 1;
	unsigned is_null : 1;
	unsigned has_member_name : 1;
	unsigned calling_convention : 2;
	unsigned has_var_arg : 1;
	unsigned is_variable_array : 1; /* set for variable array types where we don't know the variable size yet */
	unsigned is_variable_struct : 1;
	unsigned variable_size_known : 1; /* used for variable structs after we know the variable size */
	unsigned is_bitfield : 1;
	unsigned has_bitfield : 1;
	unsigned is_jitted : 1;
	unsigned is_packed : 1;
	unsigned is_unsigned : 1;
} CType;
/*
sizeof(ctype) = 32 ... sizeof'size_t' = 8 = 64 bits
# bits is 25 + POINTER_BITS + (1 << POINTER_BITS)
	n	1<<n	25+n+(1<<n)
	1	2	28
	2	4	31
	3	8	36
	4	16	45
	5	32	62
So I should be able to bump this up to 5 pointer dereference max without it changeing the sizeof() on 64bit systems
But really ... WHYYYY are we using bitfields to STORE A RECURSIVE STRUCTURE.
*/

#ifdef _MSC_VER
__declspec(align(16))
#endif
typedef struct {
	CType type
#ifdef __GNUC__
	  __attribute__ ((aligned(16)))
#endif
	  ;
} CData;
