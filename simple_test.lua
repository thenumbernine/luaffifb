-- I think their test.c / test.lua got too out of hand too quick
-- so here's me easing things more slowly ...
ffi = require 'ffi'
local assert = require 'ext.assert'

ffi.cdef[[
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

-- testing single arguments
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

-- testing double arguments
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
