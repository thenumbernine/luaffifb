#include <stdint.h>

int notification = 0;
extern void test() { notification = 1; }

extern void* test_vp() { return (void*)0xdeadbeef; }
extern void* test_vp2() { return test_vp2; }

// testing that return works
extern uint8_t test_u8() { return 42; }
extern int8_t test_s8() { return -42; }
extern uint16_t test_u16() { return 345; }
extern int16_t test_s16() { return -345; }
extern uint32_t test_u32() { return 67890; }
extern int32_t test_s32() { return -67890; }
extern uint64_t test_u64() { return 0x123456789; }
extern int64_t test_s64() { return -0x123456789; }
extern float test_f32() { return -123; }
extern double test_f64() { return -123; }

// testing that single arguments works
extern uint8_t test_u8_u8(uint8_t x) { return x+1; }
extern int8_t test_s8_s8(int8_t x) { return x+1; }
extern uint16_t test_u16_u16(uint16_t x) { return x+1; }
extern int16_t test_s16_s16(int16_t x) { return x+1; }
extern uint32_t test_u32_u32(uint32_t x) { return x+1; }
extern int32_t test_s32_s32(int32_t x) { return x+1; }
extern uint64_t test_u64_u64(uint64_t x) { return x+1; }
extern int64_t test_s64_s64(int64_t x) { return x+1; }
extern float test_f32_f32(float x) { return x+1; }
extern double test_f64_f64(double x) { return x+1; }

// testing that double arguments works
extern uint8_t test_u8_u8_u8(uint8_t x, uint8_t y) { return x+y+1; }
extern int8_t test_s8_s8_s8(int8_t x, int8_t y) { return x+y+1; }
extern uint16_t test_u16_u16_u16(uint16_t x, uint16_t y) { return x+y+1; }
extern int16_t test_s16_s16_s16(int16_t x, int16_t y) { return x+y+1; }
extern uint32_t test_u32_u32_u32(uint32_t x, uint32_t y) { return x+y+1; }
extern int32_t test_s32_s32_s32(int32_t x, int32_t y) { return x+y+1; }
extern uint64_t test_u64_u64_u64(uint64_t x, uint64_t y) { return x+y+1; }
extern int64_t test_s64_s64_s64(int64_t x, int64_t y) { return x+y+1; }
extern float test_f32_f32_f32(float x, float y) { return x+y+1; }
extern double test_f64_f64_f64(double x, double y) { return x+y+1; }
