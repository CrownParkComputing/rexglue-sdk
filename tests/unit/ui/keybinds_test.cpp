#include <catch2/catch_test_macros.hpp>

#include <rex/ui/keybinds.h>

TEST_CASE("Keybind toggles ignore keyboard auto-repeat", "[input]") {
  int calls = 0;
  rex::ui::RegisterBind("test_toggle_bind", "F5", "Test toggle", [&calls] { ++calls; });

  rex::ui::KeyEvent down(nullptr, rex::ui::VirtualKey::kF5, 1, false, false, false, false, false);
  rex::ui::KeyEvent repeat(nullptr, rex::ui::VirtualKey::kF5, 1, true, false, false, false, false);
  REQUIRE(rex::ui::ProcessKeyEvent(down));
  CHECK(calls == 1);
  CHECK_FALSE(rex::ui::ProcessKeyEvent(repeat));
  CHECK(calls == 1);

  rex::ui::KeyEvent second_down(nullptr, rex::ui::VirtualKey::kF5, 1, false, false, false, false,
                                false);
  REQUIRE(rex::ui::ProcessKeyEvent(second_down));
  CHECK(calls == 2);
  rex::ui::UnregisterBind("test_toggle_bind");
}
