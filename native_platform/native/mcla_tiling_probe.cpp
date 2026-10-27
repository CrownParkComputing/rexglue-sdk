#include "graphics_adapters/mcla_tiling_policy.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

int main() {
  using namespace mcla::native;
  static_assert(MainSceneTileDemand(1280,720,2) == 2944);
  static_assert(MainSceneTileDemand(1280,720,3) == 5888);
  static_assert(MainSceneTileDemand(1920,1080,2) == 0);
  static_assert(MainSceneTileDemand(UINT32_MAX,UINT32_MAX,0) == 0);
  static_assert(!UseSingleTile(true,2048,true,1280,720,2,2));
  static_assert(!UseSingleTile(true,4096,false,1280,720,2,2));
  static_assert(!UseSingleTile(true,4096,true,1280,720,2,1));
  static_assert(UseSingleTile(true,4096,true,1280,720,2,2));
  static_assert(UseSingleTile(true,8192,true,1280,720,4,3));
  static_assert(!UseSingleTile(true,4096,true,1280,720,4,3));
  static_assert(SurfaceFits(8192,2944,2944,5888));
  static_assert(!SurfaceFits(8192,4096,1,4097));
  static_assert(SurfaceFits(4096,4095,1,4096));
  static_assert(!SurfaceFits(4096,4095,2,4097));
  static_assert(!SurfaceFits(4096,UINT32_MAX,2,1));
  static_assert(!SurfaceFits(4096,10,2,11));
  constexpr size_t size = 0x83000000ull;
  auto* memory = static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,
      MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
  assert(memory != MAP_FAILED);
  const auto write = [&](uint32_t address,uint32_t value) {
    value=__builtin_bswap32(value);std::memcpy(memory+address,&value,4);
  };
  const auto count = [&] {uint32_t v;std::memcpy(&v,memory+0x827D42A4,4);return __builtin_bswap32(v);};
  write(0x8217A700,0x3967009F);write(0x8217A704,0x3948001F);write(0x82410E34,0x2B0B0800);
  unsetenv("XN_MCLA_TILE_TRACE");setenv("XN_MCLA_SINGLE_TILE","1",1);
  write(0x827D42A4,2);
  MclaNativeConfigureTiling(memory,2048);
  assert(!MclaNativePrepareTile(0,1280,720,2,1280,360,memory));assert(count()==2);
  assert(!MclaNativeAllowSurface(2000,944,2944));
  MclaNativeConfigureTiling(memory,4096);
  assert(!MclaNativePrepareTile(7,1280,720,2,1280,360,memory));assert(count()==2);
  assert(!MclaNativePrepareTile(0,1280,720,3,1280,360,memory));assert(count()==2);
  assert(MclaNativePrepareTile(0,1280,720,2,1280,360,memory));assert(count()==1);
  write(0x827D42A4,2);write(0x8287E064,0x827D0000);
  write(0x827D0000+396,0xBEAE7A40);write(0x827D0000+400,0xBEAE79D0);
  assert(MclaNativePrepareTile(0xBEAE7A40,1280,720,2,1280,512,memory));assert(count()==1);
  write(0x827D42A4,3);MclaNativeConfigureTiling(memory,8192);
  assert(MclaNativePrepareTile(0xBEAE79D0,1280,720,4,1280,256,memory));assert(count()==1);
  write(0x827D42A4,3);
  assert(!MclaNativePrepareTile(0xBD1F3C10,1280,720,4,1280,256,memory));assert(count()==3);
  assert(MclaNativeAllowSurface(2000,944,2944));
  assert(!MclaNativeAllowSurface(4096,200,4296));
  write(0x827D42A4,2);write(0x8217A704,0);
  MclaNativeConfigureTiling(memory,4096);
  assert(!MclaNativePrepareTile(0,1280,720,2,1280,360,memory));assert(count()==2);
  write(0x8217A704,0x3948001F);setenv("XN_MCLA_SINGLE_TILE","0",1);
  MclaNativeConfigureTiling(memory,4096);
  assert(!MclaNativePrepareTile(0,1280,720,2,1280,360,memory));assert(count()==2);
  unsetenv("XN_MCLA_SINGLE_TILE");
  write(0x827D42A4,3);
  MclaNativeConfigureTiling(memory,8192,true);
  assert(MclaNativePrepareTile(0xBEAE79D0,1280,720,4,1280,256,memory));assert(count()==1);
  assert(munmap(memory,size)==0);
}
