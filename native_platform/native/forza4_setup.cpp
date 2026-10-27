#include "forza4_setup.h"
#include "native_disc2.h"
#include "native_settings.h"
#include "runtime/kernel/local_profile.h"
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <format>
#include <iterator>
#include <spawn.h>
#include <stdexcept>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace forza4 {
namespace fs = std::filesystem;
namespace {
constexpr const char* kXexSha = "3618e85af83e972ddcc1b0d0dd808e78e29ab4ab3890a15b31c210a6a3e93e09";
constexpr std::array<const char*,4> kMovies = {"t10_ms_combined","dolby_corona_intro","forza_intro","IE_Welcome_Video"};

bool ValidName(const std::string& name) {
  return !name.empty() && name.size() <= 15 && name.front() != ' ' && name.back() != ' ' &&
      std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}

// argv goes directly to execvp. User paths are never interpreted by a shell.
bool Execute(const std::vector<std::string>& args, std::string& output, std::string& error,
             const std::function<void(const std::string&)>& progress = {}) {
  int pipefd[2];
  if (pipe(pipefd) != 0) { error = std::strerror(errno); return false; }
  std::vector<char*> argv;
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions,pipefd[0]);
  posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions,pipefd[1]);
  pid_t pid=0;
  const int spawn_error=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_error != 0) {
    error=std::strerror(spawn_error); close(pipefd[0]); close(pipefd[1]); return false;
  }
  close(pipefd[1]);
  std::array<char,4096> buffer;
  std::string line;
  ssize_t size;
  while ((size = read(pipefd[0], buffer.data(), buffer.size())) > 0) {
    // Keep diagnostics bounded if an external tool is unexpectedly verbose.
    if (output.size() < 65536) output.append(buffer.data(), std::min<size_t>(size, 65536-output.size()));
    if (progress) for (ssize_t i=0;i<size;++i) {
      if (buffer[i]=='\n') { if (!line.empty()) progress(line); line.clear(); }
      else if (line.size()<1024 && buffer[i]!='\r') line.push_back(buffer[i]);
    }
  }
  if (progress && !line.empty()) progress(line);
  close(pipefd[0]);
  int status=0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    error = std::strerror(errno); return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = output.empty() ? "The import tool failed." : output;
    return false;
  }
  return true;
}

bool FmvReady(const fs::path& root) {
  for (const auto* name : kMovies) {
    const fs::path file = root/(std::string(name)+".yuv");
    std::error_code ec;
    const auto size=fs::file_size(file,ec);
    if (ec || !size || size % 1382400) return false;
  }
  return true;
}

bool Within(const fs::path& child, const fs::path& parent) {
  auto a=child.begin(), b=parent.begin();
  for (;b!=parent.end();++a,++b)
    if (a==child.end() || *a!=*b) return false;
  return true;
}

bool Overlaps(const fs::path& a, const fs::path& b) {
  return Within(a,b) || Within(b,a);
}

fs::path Normalized(const fs::path& path) { return fs::weakly_canonical(fs::absolute(path)); }

bool OwnedRootValid(const fs::path& root, const fs::path& profile) {
  const fs::path marker=root/".forza4-setup-owned";
  const std::string expected="FM4-SETUP-v1\n"+profile.string()+"\n";
  std::ifstream file(marker);
  const std::string actual((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
  return actual==expected;
}

void PrepareOwnedRoot(const fs::path& root, const fs::path& profile) {
  const fs::path marker=root/".forza4-setup-owned";
  const std::string expected="FM4-SETUP-v1\n"+profile.string()+"\n";
  if (fs::exists(root)) {
    if (!fs::is_directory(root)) throw std::runtime_error("The local files destination is not a folder.");
    if (!fs::is_empty(root)) {
      if (!OwnedRootValid(root,profile))
        throw std::runtime_error("The local files destination contains other files. Choose an empty folder or a previous Forza setup folder.");
      return;
    }
  } else fs::create_directories(root);
  const fs::path temporary=root/".forza4-setup-owned.tmp";
  {
    std::ofstream file(temporary,std::ios::trunc);
    file << expected;file.flush();
    if (!file) throw std::runtime_error("Could not mark the local files folder as managed.");
  }
  fs::rename(temporary,marker);
}

void MaterializeFmvCache(const fs::path& cache, const fs::path& destination,
                         const std::function<void(const std::string&)>& progress) {
  fs::create_directories(destination);
  for (const auto* name : kMovies) {
    const auto filename=std::string(name)+".yuv";
    const auto from=cache/filename, to=destination/filename;
    std::error_code ec;
    const auto current_size=fs::file_size(to,ec);
    if (!ec && current_size>0 && current_size%1382400==0) continue;
    if (progress) progress("Preparing Forza video: "+filename);
    const fs::path temp=destination/(filename+".tmp."+std::to_string(getpid()));
    fs::remove(temp,ec);
    ec.clear();fs::create_hard_link(from,temp,ec);
    if (ec) { ec.clear();fs::copy_file(from,temp,fs::copy_options::none,ec); }
    if (ec) throw std::runtime_error("Could not copy the prepared Forza videos: "+ec.message());
    fs::rename(temp,to);
  }
}

bool HasDisc2Packages(const fs::path& root) {
  std::array<bool,4> found{};
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;
       !ec && it!=end;it.increment(ec)) {
    if (it.depth()>8) it.disable_recursion_pending();
    if (!it->is_regular_file(ec)) continue;
    const auto name=it->path().filename().string();
    for (int i=0;i<4;++i)
      if (name==std::format("4d530910{:08x}",i+1)) found[i]=true;
  }
  return std::all_of(found.begin(),found.end(),[](bool v){return v;});
}

void Report(const std::function<void(const std::string&)>& progress, const std::string& s) {
  if (progress) progress(s);
}

bool SaveReceipt(const fs::path& profile_root, const ForzaSetupReceipt& receipt,
                 std::string& error) {
  const fs::path path=profile_root/"forza4-setup.toml";
  const fs::path temporary=profile_root/"forza4-setup.toml.tmp";
  toml::table document{
    {"version", 1}, {"complete", true},
    {"game_root", receipt.game_root.string()},
    {"fmv_root", receipt.fmv_root.string()},
    {"disc2_root", receipt.disc2_root.string()},
    {"save_root", receipt.save_root.string()},
    {"disc2_installed", receipt.disc2_installed},
    {"source_reference", receipt.source_reference.string()},
    {"reference_url", receipt.reference_url},
    {"username", receipt.username}
  };
  fs::create_directories(profile_root);
  {
    std::ofstream file(temporary,std::ios::trunc);
    file << document << '\n'; file.flush();
    if (!file) { error="Could not save setup receipt."; return false; }
  }
  std::error_code ec;
  fs::rename(temporary,path,ec);
  if (ec) { error=ec.message(); return false; }
  return true;
}
} // namespace

bool ValidateForzaGameRoot(const fs::path& game_root, std::string& error) {
  std::error_code ec;
  if (!fs::is_regular_file(game_root/"default.xex",ec) || !fs::is_directory(game_root/"Media",ec) ||
      !fs::is_regular_file(game_root/"Media/zipmanifest.xml",ec)) {
    error="Choose the Forza Motorsport 4 Disc 1 folder with default.xex and Media/.";
    return false;
  }
  std::string output;
  if (!Execute({"sha256sum",(game_root/"default.xex").string()},output,error)) return false;
  if (output.rfind(kXexSha,0) != 0) {
    error="The selected default.xex does not match the supported Forza Motorsport 4 release.";
    return false;
  }
  return true;
}

bool LoadForzaSetup(const fs::path& profile_root, ForzaSetupReceipt& receipt, std::string& error) {
  error.clear();
  const fs::path path=profile_root/"forza4-setup.toml";
  std::error_code ec;
  if (!fs::exists(path,ec)) return false;
  auto parsed=toml::parse_file(path.string());
  if (!parsed) { error="Setup receipt is unreadable."; return false; }
  const auto& t=parsed.table();
  if (t["version"].value_or(0)!=1 || !t["complete"].value_or(false)) {
    error="Setup receipt has an unsupported format."; return false;
  }
  ForzaSetupReceipt value;
  value.game_root=t["game_root"].value_or(std::string{});
  value.fmv_root=t["fmv_root"].value_or(std::string{});
  value.disc2_root=t["disc2_root"].value_or(std::string{});
  value.save_root=t["save_root"].value_or(std::string{});
  value.disc2_installed=t["disc2_installed"].value_or(false);
  value.source_reference=t["source_reference"].value_or(std::string{});
  value.reference_url=t["reference_url"].value_or(std::string{});
  value.username=t["username"].value_or(std::string{});
  if (!ValidName(value.username) || !ValidateForzaGameRoot(value.game_root,error) ||
      !FmvReady(value.fmv_root)) {
    if (error.empty()) error="Setup receipt references missing prepared videos or an invalid user.";
    return false;
  }
  if (value.disc2_installed) for (int i=1;i<=4;++i) {
    const fs::path marker=value.save_root/"content/00000002"/
        std::format("4d530910{:08x}",i)/"native-disc2.complete";
    if (!fs::is_regular_file(marker)) {
      error="Disc 2 content is missing; reopen setup to reinstall it.";
      return false;
    }
  }
  receipt=std::move(value);
  return true;
}

std::string ForzaSetupPlayer(const fs::path& profile_root, std::string& error) {
  xn::LocalProfile profile;
  if (!profile.Open(profile_root,error)) return {};
  return profile.gamertag();
}

ForzaSetupResult RunForzaSetup(const ForzaSetupPaths& paths, const ForzaSetupRequest& request,
                               const std::function<void(const std::string&)>& progress) {
  ForzaSetupResult result;
  try {
    if (!ValidName(request.username)) throw std::runtime_error(
        "Use 1–15 printable characters without leading or trailing spaces.");
    if (paths.profile_root.empty() || (paths.save_root.empty() && request.save_root.empty()) ||
        request.disc1_source.empty())
      throw std::runtime_error("Choose a Disc 1 source and a writable profile and save location.");
    const fs::path source=Normalized(request.disc1_source);
    if (!fs::exists(source)) throw std::runtime_error("The Disc 1 source does not exist.");
    const fs::path profile_root=Normalized(paths.profile_root);
    const fs::path save_root=Normalized(request.save_root.empty() ? paths.save_root : request.save_root);
    const bool custom_local=!request.local_files_root.empty();
    const fs::path local_root=custom_local ? Normalized(request.local_files_root) : fs::path{};
    const bool self_source=custom_local && source==local_root/"game";
    if (custom_local) {
      if ((Overlaps(local_root,source) && !self_source) || Overlaps(local_root,save_root) ||
          Within(profile_root,local_root))
        throw std::runtime_error("The local files folder overlaps the game source, profile, or save folder.");
      if (self_source) {
        std::string validation_error;
        if (!OwnedRootValid(local_root,profile_root) ||
            !ValidateForzaGameRoot(source,validation_error))
          throw std::runtime_error("The managed local game folder is incomplete; choose the original Disc 1 source again.");
      }
    }
    if (Overlaps(save_root,source) || Within(profile_root,save_root))
      throw std::runtime_error("The save folder overlaps the game source or contains the profile.");
    if (custom_local) PrepareOwnedRoot(local_root,profile_root);
    fs::create_directories(save_root);
    fs::create_directories(paths.profile_root);
    const fs::path game=custom_local ? local_root/"game" : profile_root/"games/forza4";
    const fs::path helper=paths.tools_root/"setup_import.py";
    if (!fs::is_regular_file(helper)) throw std::runtime_error("The Forza import helper is missing.");
    Report(progress,"Importing and checking Disc 1...");
    std::string output,error;
    std::vector<std::string> import_args={"python3",helper.string(),"--source",source.string(),
        "--destination",game.string()};
    if (custom_local) import_args.push_back("--materialize");
    if (request.install_disc2 && request.disc2_source.empty()) import_args.push_back("--include-disc2");
    if (!Execute(import_args,output,error,progress)) throw std::runtime_error(error);
    if (!ValidateForzaGameRoot(game,error)) throw std::runtime_error(error);

    fs::path fmv=custom_local ? local_root/"fmvref" : paths.default_fmv_root;
    if (custom_local && !FmvReady(fmv) && FmvReady(paths.default_fmv_root)) {
      Report(progress,"Linking prepared Forza videos into your local files folder...");
      MaterializeFmvCache(paths.default_fmv_root,fmv,progress);
    }
    if (!FmvReady(fmv)) {
      fmv=custom_local ? local_root/"fmvref" : profile_root/"fmvref/forza4";
      const fs::path script=paths.tools_root/"prepare_fmv.sh";
      if (!fs::is_regular_file(script)) throw std::runtime_error("The video preparation tool is missing.");
      Report(progress,"Preparing Forza intro videos...");
      output.clear(); error.clear();
      if (!Execute({"bash",script.string(),"--root",game.string(),"--output",fmv.string()},
                   output,error,progress)) throw std::runtime_error(error);
      if (!FmvReady(fmv)) throw std::runtime_error("Video preparation did not produce all four videos.");
    }

    fs::path disc2_root=request.disc2_source.empty() ? game : Normalized(request.disc2_source);
    if (request.install_disc2 && request.disc2_source.empty() && !HasDisc2Packages(game))
      disc2_root=source;
    if (request.install_disc2 && fs::is_regular_file(disc2_root)) {
      Report(progress,"Unpacking Disc 2...");
      const fs::path extracted=custom_local ? local_root/"disc2" : profile_root/"games/forza4-disc2";
      output.clear(); error.clear();
      if (!Execute({"python3",helper.string(),"--disc2-source",disc2_root.string(),
                    "--destination",extracted.string()},output,error,progress)) throw std::runtime_error(error);
      disc2_root=extracted;
    }
    if (request.install_disc2) {
      Report(progress,"Installing Disc 2 content...");
      auto installed=InstallNativeDisc2(disc2_root,game,save_root,progress);
      if (!installed.ok) throw std::runtime_error(installed.message);
      result.disc2_ready=installed.installed+installed.existing;
      if (result.disc2_ready != 4)
        throw std::runtime_error("Disc 2 needs all four Forza content packages.");
    }

    const fs::path settings_path=profile_root/"forza4-native.toml";
    NativeSettings settings;
    if (!settings.Load(settings_path,error)) throw std::runtime_error(error);
    settings.auto_install_disc2=false;
    settings.disc2_root.clear();
    if (!settings.Save(settings_path,error)) throw std::runtime_error(error);
    Report(progress,"Saving player profile...");
    xn::LocalProfile profile;
    if (!profile.Open(profile_root,error) || !profile.SetGamertag(request.username,error))
      throw std::runtime_error(error);

    result.receipt.game_root=game;
    result.receipt.fmv_root=fmv;
    result.receipt.disc2_root=disc2_root;
    result.receipt.save_root=save_root;
    result.receipt.disc2_installed=request.install_disc2;
    result.receipt.source_reference=source;
    result.receipt.reference_url="https://archive.org/download/mx360gcpt3-x360-ztm/Forza.Motorsport.4.USA.X360-ZTM.rar";
    result.receipt.username=request.username;
    if (!SaveReceipt(profile_root,result.receipt,error)) throw std::runtime_error(error);
    result.ok=true;
    result.message=request.install_disc2 ? "Forza Motorsport 4 and Disc 2 are ready." :
        "Forza Motorsport 4 is ready. Disc 2 was skipped.";
  } catch (const std::exception& e) {
    result.message=e.what();
  }
  Report(progress,result.message);
  return result;
}
} // namespace forza4
