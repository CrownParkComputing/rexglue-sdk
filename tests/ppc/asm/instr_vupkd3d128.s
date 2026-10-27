# vupkd3d128 dest, src, type
#
# The comments below name the PACK TYPE. The encoded operand is type << 2, which
# is what the disassembler prints (so pack type 6 appears as ",24" in generated
# code). Pack types:
#   0 = PACK_TYPE_D3DCOLOR      3 = PACK_TYPE_FLOAT16_2
#   1 = PACK_TYPE_SHORT_2       5 = PACK_TYPE_FLOAT16_4
#   2 = PACK_TYPE_NORMPACKED32  6 = PACK_TYPE_NORMPACKED64

# vupkd3d128 is broken in binutils, so these are hand coded

test_vupkd3d128_d3dcolor:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 04010203]
  # vupkd3d128 v3, v3, 0
  .long 0x18601FF0
  blr
  #_ REGISTER_OUT v3 [3f800001, 3f800002, 3f800003, 3f800004]

test_vupkd3d128_short2_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 7FFF8001]
  # vupkd3d128 v3, v3, 1
  .long 0x18641FF0
  blr
  #_ REGISTER_OUT v3 [40407fff, 403f8001, 00000000, 3f800000]
test_vupkd3d128_short2_1:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 4000C000]
  # vupkd3d128 v3, v3, 1
  .long 0x18641FF0
  blr
  #_ REGISTER_OUT v3 [40404000, 403FC000, 00000000, 3f800000]
test_vupkd3d128_short2_2:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 7FFFF333]
  # vupkd3d128 v3, v3, 1
  .long 0x18641FF0
  blr
  #_ REGISTER_OUT v3 [40407FFF, 403FF333, 00000000, 3f800000]
test_vupkd3d128_short2_3:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 00008000]
  # vupkd3d128 v3, v3, 1
  .long 0x18641FF0
  blr
  #_ REGISTER_OUT v3 [40400000, 7FC00000, 00000000, 3f800000]

test_vupkd3d128_short4_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 7FFFFFFF, 007FFFF8]
  # vupkd3d128 v3, v3, 4
  .long 0x18701FF0
  blr
  #_ REGISTER_OUT v3 [40407FFF, 403FFFFF, 4040007F, 403FFFF8]

test_vupkd3d128_float16_2_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 3800B800]
  # vupkd3d128 v3, v3, 3
  .long 0x186C1FF0
  blr
  #_ REGISTER_OUT v3 [3F000000, BF000000, 00000000, 3f800000]

test_vupkd3d128_float16_4_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 3800B801, 3802B803]
  # vupkd3d128 v3, v3, 5
  .long 0x18741FF0
  blr
  #_ REGISTER_OUT v3 [3F000000, bf002000, 3f004000, bf006000]

test_vupkd3d128_uint_2101010_0:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 400001FF]
  # vupkd3d128 v3, v3, 2
  .long 0x18681FF0
  blr
  #_ REGISTER_OUT v3 [404001FF, 40400000, 40400000, 3F800001]
test_vupkd3d128_uint_2101010_1:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 40000201]
  # vupkd3d128 v3, v3, 2
  .long 0x18681FF0
  blr
  #_ REGISTER_OUT v3 [403FFE01, 40400000, 40400000, 3F800001]
test_vupkd3d128_uint_2101010_2:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, CDCDCDCD, 40000200]
  # vupkd3d128 v3, v3, 2
  .long 0x18681FF0
  blr
  #_ REGISTER_OUT v3 [7FC00000, 40400000, 40400000, 3F800001]

# PACK_TYPE_NORMPACKED64 (encoded ,24): w(4):z(20):y(20):x(20).
# The packed value is the guest's LOW doubleword, so it spans guest elements
# 2 and 3; guest elements 0-1 are poisoned here to catch a read of the wrong
# half. x/y/z decode to the 3.0+X form, w to the 1.0+w form.

test_vupkd3d128_normpacked64_simple:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 40000300, 00200001]
  # vupkd3d128 v3, v3, 6        x=1 y=2 z=3 w=4
  .long 0x18781FF0
  blr
  #_ REGISTER_OUT v3 [40400001, 40400002, 40400003, 3F800004]

test_vupkd3d128_normpacked64_negative:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, FFFFFDFF, FFEFFFFF]
  # vupkd3d128 v3, v3, 6        x=-1 y=-2 z=-3 w=15
  .long 0x18781FF0
  blr
  #_ REGISTER_OUT v3 [403FFFFF, 403FFFFE, 403FFFFD, 3F80000F]

test_vupkd3d128_normpacked64_extremes:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 0000007F, FFF80000]
  # vupkd3d128 v3, v3, 6        x=-524288 y=524287 z=0 w=0
  .long 0x18781FF0
  blr
  #_ REGISTER_OUT v3 [40380000, 4047FFFF, 40400000, 3F800000]

test_vupkd3d128_normpacked64_zero:
  #_ REGISTER_IN v3 [CDCDCDCD, CDCDCDCD, 00000000, 00000000]
  # vupkd3d128 v3, v3, 6        x=0 y=0 z=0 w=0
  .long 0x18781FF0
  blr
  #_ REGISTER_OUT v3 [40400000, 40400000, 40400000, 3F800000]
