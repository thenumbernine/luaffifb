-- I think their test.c / test.lua got too out of hand too quick
-- so here's me easing things more slowly ...
ffi = require 'ffi'
local assert = require 'ext.assert'

ffi.cdef[[

int var;
void test();

void* test_vp();
void* test_vp2();

uint8_t test_u8();
int8_t test_s8();
uint16_t test_u16();
int16_t test_s16();
uint32_t test_u32();
int32_t test_s32();
uint64_t test_u64();
int64_t test_s64();
float test_f32();
double test_f64();

void* test_vp_sz(size_t x);

void test_v_u8(uint8_t x);
void test_v_s8(int8_t x);
void test_v_u16(uint16_t x);
void test_v_s16(int16_t x);
void test_v_u32(uint32_t x);
void test_v_s32(int32_t x);
void test_v_u64(uint64_t x);
void test_v_s64(int64_t x);
void test_v_f32(float x);
void test_v_f64(double x);

uint8_t test_u8_u8(uint8_t x);
int8_t test_s8_s8(int8_t x);
uint16_t test_u16_u16(uint16_t x);
int16_t test_s16_s16(int16_t x);
uint32_t test_u32_u32(uint32_t x);
int32_t test_s32_s32(int32_t x);
uint64_t test_u64_u64(uint64_t x);
int64_t test_s64_s64(int64_t x);
float test_f32_f32(float x);
double test_f64_f64(double x);

uint8_t test_u8_u8_u8(uint8_t x, uint8_t y);
int8_t test_s8_s8_s8(int8_t x, int8_t y);
uint16_t test_u16_u16_u16(uint16_t x, uint16_t y);
int16_t test_s16_s16_s16(int16_t x, int16_t y);
uint32_t test_u32_u32_u32(uint32_t x, uint32_t y);
int32_t test_s32_s32_s32(int32_t x, int32_t y);
uint64_t test_u64_u64_u64(uint64_t x, uint64_t y);
int64_t test_s64_s64_s64(int64_t x, int64_t y);
float test_f32_f32_f32(float x, float y);
double test_f64_f64_f64(double x, double y);
]]

local lib = ffi.load'libsimple_test.so'

for rep=1,5 do
print('!!!!!!!! BEGINNING REPEAT '..rep..' !!!!!!!!')
--[=[	
	-- how to test void() functions... state change
	assert.eq(lib.var, 0)
	lib.test()
	assert.eq(lib.var, 1)
	lib.var = 0

	assert.eq(lib.test_vp(), ffi.new('void*', 0xdeadbeef))

	--[[ TODO FIXME casting C functions to cdata<void*> gives "unable to convert argument 2 from lua<function> to cdata<pointer>" from ffi.c type_error()
	-- But in luajit it works
	-- Because in our current implementation we are converting the C function to a lua_CFunction that's being called
	-- To get this to work, we need to return CData of the C function and do this all in cdata_call ...
	assert.eq(lib.test_vp2(), ffi.cast('void*', lib.test_vp2))	-- TODO can't
	--]]

	-- testing return
	assert.eq(lib.test_u8(), 42)
	assert.eq(lib.test_s8(), -42)
	assert.eq(lib.test_u16(), 345)
	assert.eq(lib.test_s16(), -345)
	assert.eq(lib.test_u32(), 67890)
	assert.eq(lib.test_s32(), -67890)
	assert.eq(lib.test_u64(), ffi.new('uint64_t', 0x123456789))
	assert.eq(lib.test_s64(), ffi.new('int64_t', -0x123456789))
	assert.eq(lib.test_f32(), -123)	-- at what point does this become testing floating point parsing accuracy ...
	assert.eq(lib.test_f64(), -123)	-- at what point does this become testing floating point parsing accuracy ...

	-- testing return & single arguments
--]=]
	lib.test_v_u8(42) assert.eq(lib.var, 42+1)
--[=[	
	lib.test_v_s8(-42) assert.eq(lib.var, -42+1)
	lib.test_v_u16(345) assert.eq(lib.var, 345+1)
	lib.test_v_s16(-345) assert.eq(lib.var, -345+1)
	lib.test_v_u32(67890) assert.eq(lib.var, 67890+1)
	lib.test_v_s32(-67890) assert.eq(lib.var, -67890+1)
	lib.test_v_u64(ffi.new('uint64_t', 0x123456789)) assert.eq(lib.var, 0x23456789+1)
	lib.test_v_s64(ffi.new('int64_t', -0x123456789)) assert.eq(lib.var, -0x23456789+1)
--]=]	
	lib.test_v_f32(-123) assert.eq(lib.var, -123+1)	-- at what point does this become testing floating point parsing accuracy ...
--[=[
	lib.test_v_f64(-123) assert.eq(lib.var, -123+1)	-- at what point does this become testing floating point parsing accuracy ...

	
	-- testing return & single arguments
	-- TODO if I run this *block* multiple times then I get memory corruption problems somewhere deep inside libffi
	assert.eq(lib.test_u8_u8(42), 42+1)
	assert.eq(lib.test_s8_s8(-42), -42+1)
	assert.eq(lib.test_u16_u16(345), 345+1)
	assert.eq(lib.test_s16_s16(-345), -345+1)
	assert.eq(lib.test_u32_u32(67890), 67890+1)
	assert.eq(lib.test_s32_s32(-67890), -67890+1)
	assert.eq(lib.test_u64_u64(ffi.new('uint64_t', 0x123456789)), ffi.new('uint64_t', 0x123456789)+1)
	assert.eq(lib.test_s64_s64(ffi.new('int64_t', -0x123456789)), ffi.new('int64_t', -0x123456789)+1)
	assert.eq(lib.test_f32_f32(-123), -123+1)	-- at what point does this become testing floating point parsing accuracy ...
	assert.eq(lib.test_f64_f64(-123), -123+1)	-- at what point does this become testing floating point parsing accuracy ...

	-- testing return & double arguments
	assert.eq(lib.test_u8_u8_u8(42, 42), (42)+(42)+1)
	assert.eq(lib.test_s8_s8_s8(-42, -42), (-42)+(-42)+1)
	assert.eq(lib.test_u16_u16_u16(345, 345), (345)+(345)+1)
	assert.eq(lib.test_s16_s16_s16(-345, -345), (-345)+(-345)+1)
	assert.eq(lib.test_u32_u32_u32(67890, 67890), (67890)+(67890)+1)
	assert.eq(lib.test_s32_s32_s32(-67890, -67890), (-67890)+(-67890)+1)
	assert.eq(lib.test_u64_u64_u64(ffi.new('uint64_t', 0x123456789), ffi.new('uint64_t', 0x123456789)), ffi.new('uint64_t', (0x123456789)+(0x123456789)+1))
	assert.eq(lib.test_s64_s64_s64(ffi.new('int64_t', -0x123456789), ffi.new('int64_t', -0x123456789)), ffi.new('int64_t', (-0x123456789)+(-0x123456789)+1))
	assert.eq(lib.test_f32_f32_f32(-123, -123), (-123)+(-123)+1)	-- at what point does this become testing floating point parsing accuracy ...
	assert.eq(lib.test_f64_f64_f64(-123, -123), (-123)+(-123)+1)	-- at what point does this become testing floating point parsing accuracy ...
--]=]
end
