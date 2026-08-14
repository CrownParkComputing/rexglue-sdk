#include <catch2/catch_test_macros.hpp>

#include <rex/audio/xma/register_file.h>

TEST_CASE("XMA observed lock-control register is classified and retains writes", "[audio][xma]") {
  rex::audio::XmaRegisterFile registers;
  const auto* info = registers.GetRegisterInfo(rex::audio::XmaRegister::ObservedLockControl);
  REQUIRE(info != nullptr);
  CHECK(std::string_view(info->name) == "ObservedLockControl");

  registers[rex::audio::XmaRegister::ObservedLockControl] = 0x02000000;
  CHECK(registers[rex::audio::XmaRegister::ObservedLockControl] == 0x02000000);
  registers[rex::audio::XmaRegister::ObservedLockControl] = 0x03000000;
  CHECK(registers[rex::audio::XmaRegister::ObservedLockControl] == 0x03000000);
}
