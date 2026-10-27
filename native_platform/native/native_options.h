#pragma once
#include <SDL3/SDL.h>
#include <filesystem>
#include <memory>
#include <string>
#include "native_settings.h"
#include "forza4_setup.h"
namespace xn::gfx { class VulkanRenderer; }
namespace forza4 {
struct NativeOptionsConfig {
  std::string title = "FORZA MOTORSPORT 4 | NATIVE SETTINGS";
  bool disc2 = true;
  bool player = true;
  bool setup_wizard = false;
};
bool ApplyNativeDisplay(SDL_Window*,const NativeSettings&,std::string& error);
class NativeOptions {
 public:
  explicit NativeOptions(NativeOptionsConfig config = {});
  ~NativeOptions();
  bool Initialize(SDL_Window*,xn::gfx::VulkanRenderer*,NativeSettings&,
      const std::filesystem::path& settings_path,const std::filesystem::path& game_root,
      const std::filesystem::path& save_root,std::string& error);
  bool HandleEvent(const SDL_Event&);
  void Update();
  void SetOpen(bool);
  bool IsOpen() const;
  void InstallDisc2();
  bool InstallingDisc2() const;
 private:
  friend bool RunForzaSetupWizard(SDL_Window*,xn::gfx::VulkanRenderer*,NativeSettings&,
      const ForzaSetupPaths&,ForzaSetupReceipt&,std::string&,double);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Runs before the guest boots. False with an empty error means the user left setup.
bool RunForzaSetupWizard(SDL_Window*,xn::gfx::VulkanRenderer*,NativeSettings&,
    const ForzaSetupPaths&,ForzaSetupReceipt&,std::string& error,double seconds_limit=0);
}
