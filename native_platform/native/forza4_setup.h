#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace forza4 {

struct ForzaSetupPaths {
  std::filesystem::path profile_root;
  std::filesystem::path save_root;
  std::filesystem::path current_game_root;
  std::filesystem::path default_fmv_root;
  std::filesystem::path tools_root;
};

struct ForzaSetupRequest {
  std::filesystem::path disc1_source;
  std::filesystem::path disc2_source;
  std::string username;
  bool install_disc2 = true;
  std::filesystem::path local_files_root;
  std::filesystem::path save_root;
};

struct ForzaSetupReceipt {
  std::filesystem::path game_root;
  std::filesystem::path fmv_root;
  std::filesystem::path disc2_root;
  std::filesystem::path save_root;
  std::filesystem::path source_reference;
  std::string reference_url;
  bool disc2_installed = false;
  std::string username;
};

struct ForzaSetupResult {
  bool ok = false;
  ForzaSetupReceipt receipt;
  std::string message;
  int disc2_ready = 0;
};

// A missing or invalid receipt returns false. An absent receipt leaves error empty.
bool LoadForzaSetup(const std::filesystem::path& profile_root,
                    ForzaSetupReceipt& receipt, std::string& error);
bool ValidateForzaGameRoot(const std::filesystem::path& game_root,
                           std::string& error);
std::string ForzaSetupPlayer(const std::filesystem::path& profile_root,
                             std::string& error);
ForzaSetupResult RunForzaSetup(const ForzaSetupPaths& paths,
                               const ForzaSetupRequest& request,
                               const std::function<void(const std::string&)>& progress = {});

}  // namespace forza4
