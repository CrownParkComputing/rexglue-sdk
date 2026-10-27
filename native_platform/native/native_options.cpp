#include "native_options.h"
#include "native_disc2.h"
#include "gfx/vulkan/vulkan_renderer.h"
#include "gfx/host_window.h"
#include "gfx/frame_rate.h"
#include "kernel/local_profile.h"
#include "kernel/host_input.h"
#include "audio/sdl_output.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cctype>
#include <format>
#include <mutex>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_UINT_DRAW_INDEX
#define NK_BUTTON_TRIGGER_ON_RELEASE
#define NK_IMPLEMENTATION
#include "nuklear.h"

namespace forza4 {
namespace {
bool ResizeHyprlandWindow(int width,int height) {
  const char* signature=std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  const char* runtime=std::getenv("XDG_RUNTIME_DIR");
  if(!signature || !*signature || !runtime || !*runtime ||
      std::strcmp(SDL_GetCurrentVideoDriver(),"wayland")) return false;
  const auto path=(std::filesystem::path(runtime)/"hypr"/signature/".socket.sock").string();
  sockaddr_un address{};address.sun_family=AF_UNIX;
  if(path.size()>=sizeof(address.sun_path)) return false;
  std::memcpy(address.sun_path,path.c_str(),path.size()+1);
  auto dispatch=[&](const std::string& command) {
    const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0) return false;
    const timeval timeout{0,250000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;
    if(ok) ok=send(fd,command.data(),command.size(),MSG_NOSIGNAL)==ssize_t(command.size());
    char reply[32]{};const auto count=ok?recv(fd,reply,sizeof(reply)-1,0):-1;
    if(std::getenv("XN_NATIVE_MENU_INPUT_LOG")) std::fprintf(stderr,
        "native-display IPC: %s -> count=%zd reply=%.*s errno=%d path=%s\n",
        command.c_str(),count,int(std::max<ssize_t>(0,count)),reply,errno,path.c_str());
    close(fd);return count>=2 && reply[0]=='o' && reply[1]=='k';
  };
  // Only this process's window is addressed. Some Wayland tiling state is
  // intentionally enforced by the compositor rather than SDL's size hints.
  return dispatch(std::format("/dispatch setfloating pid:{}",getpid())) &&
      dispatch(std::format("/dispatch resizewindowpixel exact {} {},pid:{}",width,height,getpid()));
}
}
bool ApplyNativeDisplay(SDL_Window* window,const NativeSettings& s,std::string& error) {
  if(!window) return true;
  std::fprintf(stderr,"native-display: mode=%d request=%dx%d driver=%s pid=%d\n",
      s.window_mode,s.width,s.height,SDL_GetCurrentVideoDriver(),getpid());
  if((SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN) &&
      (!SDL_SetWindowFullscreen(window,false) || !SDL_SyncWindow(window))) {error=SDL_GetError();return false;}
  if(s.window_mode==0) {
    // Hyprland may retain a maximized SDL flag on floating windows. Request
    // the geometry directly before issuing any maximization-state operation.
    if(ResizeHyprlandWindow(s.width,s.height)) return true;
    if((SDL_GetWindowFlags(window)&SDL_WINDOW_MAXIMIZED) &&
        (!SDL_RestoreWindow(window) || !SDL_SyncWindow(window))) {error=SDL_GetError();return false;}
    if(!SDL_SetWindowFullscreenMode(window,nullptr) || !SDL_SetWindowSize(window,s.width,s.height) || !SDL_SyncWindow(window)) {
      error=SDL_GetError();return false;
    }
  } else {
    SDL_DisplayMode closest{};
    const SDL_DisplayMode* mode=nullptr;
    if(s.window_mode==2) {
      if(!SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window),s.width,s.height,0,true,&closest)) {
        error=SDL_GetError();return false;
      }
      if(closest.w!=s.width || closest.h!=s.height) {error="This display does not offer that fullscreen resolution.";return false;}
      mode=&closest;
    }
    if(!SDL_SetWindowFullscreenMode(window,mode) || !SDL_SetWindowFullscreen(window,true)) {
      error=SDL_GetError();return false;
    }
  }
  if(!SDL_SyncWindow(window)) {error="The display server did not confirm the requested mode.";return false;}
  if(s.window_mode==0) {
    int w=0,h=0;SDL_GetWindowSize(window,&w,&h);
    if(w!=s.width || h!=s.height) {
      error="The window manager kept a different size. Float the window or use exclusive fullscreen.";return false;
    }
  }
  return true;
}
struct NativeOptions::Impl {
  NativeOptionsConfig features;
  SDL_Window* window=nullptr;
  xn::gfx::VulkanRenderer* renderer=nullptr;
  NativeSettings* settings=nullptr;
  std::filesystem::path settings_path,game_root,save_root;
  nk_context ctx{};
  nk_font_atlas atlas{};
  nk_draw_null_texture null_texture{};
  std::shared_ptr<xn::gfx::HostUiFont> font;
  bool initialized=false,open=false,pad_chord=false;
  bool initial_display_pending=false;
  uint32_t initial_display_attempts=0;
  int tab=0,mode=1,resolution=0;
  float sx=1,sy=1;
  uint64_t last_update=0,confirm_until=0;
  NativeSettings previous_display;
  std::vector<std::pair<int,int>> resolutions;
  std::array<char,4096> disc_path{};
  std::array<char,16> player_name{};
  ForzaSetupPaths setup_paths;
  ForzaSetupReceipt setup_receipt;
  std::array<char,4096> disc1_source{},disc2_source{},local_files_root{},save_games_root{};
  enum SetupPage {Welcome,OriginalFiles,Locations,PlayerName,Review,Installing,Done};
  int setup_page=Welcome;
  bool setup_done=false,setup_cancelled=false,setup_skip_disc2=false;
  std::atomic<bool> setup_running=false,setup_finished=false;
  std::thread setup_worker;
  ForzaSetupResult setup_result;
  std::mutex setup_progress_mutex;
  std::string setup_progress,setup_status;
  std::string status="Changes are saved to this native profile.";
  std::atomic<bool> installing=false;
  std::mutex progress_mutex;
  std::string disc_status="Disc 2 has not been checked in this session.";
  std::thread installer;
  SDL_Gamepad* pad=nullptr;
  float pad_x=0,pad_y=0;
  bool pad_mouse=false;
  ~Impl() {
    if(setup_worker.joinable()) setup_worker.join();
    if(installer.joinable()) installer.join();
    if(pad) SDL_CloseGamepad(pad);
    if(initialized) {nk_free(&ctx);nk_font_atlas_clear(&atlas);}
  }
  bool Save() {
    std::string error;
    auto saved=*settings;
    if(confirm_until) {
      saved.width=previous_display.width;saved.height=previous_display.height;
      saved.window_mode=previous_display.window_mode;
    }
    if(!saved.Save(settings_path,error)) {status="Could not save: "+error;return false;}
    return true;
  }
  void Apply() {
    xn::SetFpsOverlayEnabled(settings->show_fps);
    xn::audio::SetMasterVolume(settings->volume);
    renderer->SetPresentationSettings({settings->vsync,settings->filter,settings->sharpness});
    renderer->SetFrameRateTarget(settings->fps);
  }
  void Open(bool value) {
    if(open==value) return;
    open=value;xn::SetHostInputCaptured(open);
    if(open) {
      std::memset(&ctx.input,0,sizeof(ctx.input));
      mode=settings->window_mode;
      auto it=std::find(resolutions.begin(),resolutions.end(),std::pair(settings->width,settings->height));
      resolution=it==resolutions.end()?0:int(it-resolutions.begin());
      if(!pad) {
        int count=0; auto* ids=SDL_GetGamepads(&count);
        if(count>0) pad=SDL_OpenGamepad(ids[0]);SDL_free(ids);
      }
      SDL_StartTextInput(window);
    } else {SDL_StopTextInput(window);renderer->SetHostOverlay(nullptr);}
  }
  bool ValidSetupSource(const char* source,bool required) {
    if(!*source) return !required;
    const std::filesystem::path path(source);
    std::error_code ec;
    if(!std::filesystem::exists(path,ec)) return false;
    if(std::filesystem::is_directory(path,ec)) return true;
    if(!std::filesystem::is_regular_file(path,ec)) return false;
    auto ext=path.extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    return ext==".iso" || ext==".xiso" || ext==".rar" || ext==".zip" || ext==".7z";
  }
  bool ValidSetupName() {
    const auto n=std::strlen(player_name.data());
    if(n<1 || n>15 || player_name[0]==' ' || player_name[n-1]==' ') return false;
    for(size_t i=0;i<n;++i) if(static_cast<unsigned char>(player_name[i])<32 ||
        static_cast<unsigned char>(player_name[i])>126) return false;
    return true;
  }
  void StartSetup() {
    if(setup_running) return;
    if(setup_worker.joinable()) {
      if(!setup_finished) return;
      setup_worker.join();
    }
    ForzaSetupRequest request;
    request.disc1_source=disc1_source.data();request.disc2_source=disc2_source.data();
    request.username=player_name.data();request.install_disc2=!setup_skip_disc2;
    request.local_files_root=local_files_root.data();request.save_root=save_games_root.data();
    setup_progress="Preparing original files...";setup_status.clear();setup_page=Installing;
    setup_finished=false;setup_running=true;
    setup_worker=std::thread([this,request=std::move(request)] {
      try {
        setup_result=RunForzaSetup(setup_paths,request,[this](const std::string& message) {
          std::lock_guard lock(setup_progress_mutex);setup_progress=message;
        });
      } catch(const std::exception& e) {setup_result={};setup_result.message=e.what();}
      catch(...) {setup_result={};setup_result.message="Setup stopped unexpectedly.";}
      setup_running=false;setup_finished=true;
    });
  }
  void BrowseSetup(int target,bool folder) {
    auto state=dialog;
    {
      std::lock_guard lock(state->mutex);
      if(state->pending) return;
      state->pending=true;state->target=target;state->result.clear();
    }
    const char* initial=target==1?disc1_source.data():target==2?disc2_source.data():
        target==3?local_files_root.data():save_games_root.data();
    auto callback=[](void* data,const char* const* paths,int) {
      std::unique_ptr<std::shared_ptr<DialogState>> holder(static_cast<std::shared_ptr<DialogState>*>(data));
      auto& state=**holder;std::lock_guard lock(state.mutex);
      if(paths && paths[0]) state.result=paths[0];
      state.pending=false;
    };
    auto* holder=new std::shared_ptr<DialogState>(state);
    std::filesystem::path location(initial);
    std::error_code ec;
    if(std::filesystem::is_regular_file(location,ec)) location=location.parent_path();
    while(!location.empty() && !std::filesystem::is_directory(location,ec)) {
      const auto parent=location.parent_path();
      if(parent==location) break;
      location=parent;
    }
    const auto location_string=location.string();
    const char* initial_folder=location_string.empty()?nullptr:location_string.c_str();
    if(folder) SDL_ShowOpenFolderDialog(callback,holder,window,initial_folder,false);
    else {
      static constexpr SDL_DialogFileFilter filters[]={
          {"Disc images and archives","iso;xiso;rar;zip;7z"}};
      SDL_ShowOpenFileDialog(callback,holder,window,filters,1,initial_folder,false);
    }
  }
  void SetupWizard() {
    static constexpr const char* pages[]={"Welcome","Original files","Locations","Player name","Review","Install","Ready"};
    const float line=36;
    const float available_height=nk_window_get_content_region(&ctx).h;
    Text(std::format("Step {} of 7  |  {}",setup_page+1,pages[setup_page]),line);
    nk_layout_row_dynamic(&ctx,8,1);nk_spacing(&ctx,1);
    // Reserve the header, status, footer, row gaps and window padding from the
    // actual Nuklear content area. Only the page body scrolls.
    nk_layout_row_dynamic(&ctx,std::max(120.f,available_height-210.f),1);
    if(nk_group_begin(&ctx,"setup-page-content",NK_WINDOW_BORDER)) {
    switch(setup_page) {
      case Welcome:
        Text("Welcome to Forza Motorsport 4",54);
        Text("This setup prepares your own original game files, local player profile, and optional Disc 2 content before the game starts.",70);
        Text("Reference source used for this port:",line);
        Text("archive.org/download/mx360gcpt3-x360-ztm",line);
        Text("Expected archive: Forza.Motorsport.4.USA.X360-ZTM.rar",line);
        nk_layout_row_dynamic(&ctx,line,1);
        if(nk_button_label(&ctx,"Open reference source page"))
          SDL_OpenURL("https://archive.org/download/mx360gcpt3-x360-ztm");
        break;
      case OriginalFiles: {
        Text("Disc 1: select an extracted game folder or an ISO, XISO, RAR, ZIP, or 7z file.",54);
        nk_layout_row_dynamic(&ctx,line,1);
        nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,disc1_source.data(),disc1_source.size(),nk_filter_default);
        nk_layout_row_dynamic(&ctx,line,2);
        if(nk_button_label(&ctx,"Browse Disc 1 folder")) BrowseSetup(1,true);
        if(nk_button_label(&ctx,"Browse Disc 1 file")) BrowseSetup(1,false);
        nk_bool skip=setup_skip_disc2;
        nk_layout_row_dynamic(&ctx,line,1);
        nk_checkbox_label(&ctx,"Skip Disc 2 for now",&skip);setup_skip_disc2=skip;
        if(!setup_skip_disc2) {
          Text("Disc 2: select its folder or archive, or leave blank to detect it in Disc 1.",48);
          nk_layout_row_dynamic(&ctx,line,1);
          nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,disc2_source.data(),disc2_source.size(),nk_filter_default);
          nk_layout_row_dynamic(&ctx,line,2);
          if(nk_button_label(&ctx,"Browse Disc 2 folder")) BrowseSetup(2,true);
          if(nk_button_label(&ctx,"Browse Disc 2 file")) BrowseSetup(2,false);
        }
        break;
      }
      case Locations:
        Text("Local game files: where imported game, Disc 2, and video files will be stored.",54);
        nk_layout_row_dynamic(&ctx,line,1);
        nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,local_files_root.data(),local_files_root.size(),nk_filter_default);
        nk_layout_row_dynamic(&ctx,line,1);
        if(nk_button_label(&ctx,"Browse local files folder")) BrowseSetup(3,true);
        Text("Saved games / installed content: choose where your driver saves and installed content will be stored.",70);
        nk_layout_row_dynamic(&ctx,line,1);
        nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,save_games_root.data(),save_games_root.size(),nk_filter_default);
        nk_layout_row_dynamic(&ctx,line,1);
        if(nk_button_label(&ctx,"Browse saved games folder")) BrowseSetup(4,true);
        Text("You can type a new folder path. Choose an empty folder for local files; existing saved games are kept.",54);
        break;
      case PlayerName:
        Text("Choose a local player name (1-15 ASCII characters).",54);
        nk_layout_row_dynamic(&ctx,line,1);
        nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,player_name.data(),player_name.size(),nk_filter_ascii);
        Text("Your existing profile identity and saved games are kept when the name changes.",54);
        break;
      case Review:
        Text("Check the setup choices before installing.",line);
        Text("Disc 1: "+std::string(disc1_source.data()),64);
        Text(setup_skip_disc2?"Disc 2: skipped; run --setup later to add it.":
            disc2_source[0]?"Disc 2: "+std::string(disc2_source.data()):
            "Disc 2: detect content in Disc 1 assets",64);
        Text("Player: "+std::string(player_name.data()),line);
        Text("Local game files: "+std::string(local_files_root.data()),64);
        Text("Saved games / installed content: "+std::string(save_games_root.data()),64);
        Text("The game will launch only after setup completes.",line);
        break;
      case Installing: {
        std::string progress;{std::lock_guard lock(setup_progress_mutex);progress=setup_progress;}
        Text(setup_running?"Installing original files...":setup_result.ok?"Setup completed.":"Setup could not finish.",line);
        Text(progress,90);
        if(setup_finished && !setup_result.ok) Text(setup_result.message,100);
        if(setup_running) Text("Please keep this window open until installation finishes.",54);
        break;
      }
      case Done:
        Text("Setup is ready. Launch the game with your new local profile.",70);
        Text("Game files: "+setup_receipt.game_root.string(),70);
        Text("Saved games: "+setup_receipt.save_root.string(),70);
        Text(setup_receipt.disc2_installed?"Disc 2 content installed.":"Disc 2 content can be added later with --setup.",54);
        break;
    }
    nk_group_end(&ctx);
    }
    Text(setup_status.empty()?" ":setup_status,54);
    nk_layout_row_dynamic(&ctx,40,3);
    if(setup_page>Welcome && setup_page<Installing) {
      if(nk_button_label(&ctx,"Back")) {--setup_page;setup_status.clear();}
    } else if(setup_page==Installing && setup_finished && !setup_result.ok) {
      if(nk_button_label(&ctx,"Back to review")) {setup_page=Review;setup_status.clear();}
    } else nk_label(&ctx,"",NK_TEXT_LEFT);
    if(setup_page==Done) {
      if(nk_button_label(&ctx,"Launch game")) setup_done=true;
    } else if(setup_page==Installing) {
      if(setup_finished && !setup_result.ok && nk_button_label(&ctx,"Retry install")) {
        if(setup_worker.joinable()) setup_worker.join();StartSetup();
      }
    } else if(nk_button_label(&ctx,setup_page==Review?"Install":"Continue")) {
      if(setup_page==OriginalFiles && (!ValidSetupSource(disc1_source.data(),true) ||
          (!setup_skip_disc2 && !ValidSetupSource(disc2_source.data(),false))))
        setup_status="Choose an existing Disc 1 folder or supported file. Check the optional Disc 2 path.";
      else if(setup_page==Locations && (!local_files_root[0] || !save_games_root[0]))
        setup_status="Choose both a local game files folder and a saved games folder.";
      else if(setup_page==PlayerName && !ValidSetupName())
        setup_status="Use 1-15 printable ASCII characters without leading or trailing spaces.";
      else {setup_status.clear();if(setup_page==Review) StartSetup();else ++setup_page;}
    }
    if(setup_page!=Installing) {
      if(nk_button_label(&ctx,"Exit setup")) setup_cancelled=true;
    } else nk_label(&ctx,"",NK_TEXT_LEFT);
  }
  void Text(const std::string& value,float height=30) {
    nk_layout_row_dynamic(&ctx,height,1);nk_label_wrap(&ctx,value.c_str());
  }
  void Display() {
    Text("Output resolution");
    std::vector<std::string> labels;std::vector<const char*> names;
    for(auto [w,h]:resolutions) labels.push_back(std::format("{} x {}",w,h));
    for(auto& s:labels) names.push_back(s.c_str());
    nk_layout_row_dynamic(&ctx,36,2);
    resolution=nk_combo(&ctx,names.data(),names.size(),resolution,30,nk_vec2(260,280));
    const char* modes[]={"Windowed","Desktop fullscreen","Exclusive fullscreen"};
    mode=nk_combo(&ctx,modes,3,mode,30,nk_vec2(290,160));
    Text("Desktop fullscreen uses your monitor's desktop resolution. Windowed and exclusive modes use the selected size.",50);
    nk_layout_row_dynamic(&ctx,36,1);
    if(confirm_until) {
      const auto left=int((confirm_until-SDL_GetTicks())/1000)+1;
      Text(std::format("Keep this display mode? Reverts in {} seconds.",left));
      nk_layout_row_dynamic(&ctx,36,2);
      if(nk_button_label(&ctx,"Keep display mode")) {confirm_until=0;if(Save()) status="Display mode saved.";}
      if(nk_button_label(&ctx,"Revert")) Revert();
    } else if(nk_button_label(&ctx,"Apply display mode")) {
      previous_display=*settings;
      auto [w,h]=resolutions[resolution];settings->width=w;settings->height=h;settings->window_mode=mode;
      std::string error;
      if(!ApplyNativeDisplay(window,*settings,error)) {Revert();status="Display change failed: "+error;}
      else confirm_until=SDL_GetTicks()+15000;
    }
    Text("Presentation cap");
    const char* rates[]={"30 FPS","60 FPS","120 FPS"};
    int selected=settings->fps==30?0:settings->fps==60?1:2;
    nk_layout_row_dynamic(&ctx,36,1);
    const int next=nk_combo(&ctx,rates,3,selected,30,nk_vec2(250,150));
    if(next!=selected) {settings->fps=next==0?30:next==1?60:120;Apply();Save();}
    Text("Changes presentation pacing. It does not enable frame generation or change the game's timing settings.",50);
    nk_bool vsync=settings->vsync,fps=settings->show_fps;
    nk_layout_row_dynamic(&ctx,32,2);
    nk_checkbox_label(&ctx,"VSync",&vsync);nk_checkbox_label(&ctx,"FPS counter (F8)",&fps);
    if(bool(vsync)!=settings->vsync || bool(fps)!=settings->show_fps) {
      settings->vsync=vsync;settings->show_fps=fps;Apply();Save();
    }
    Text("Menu scale");nk_layout_row_dynamic(&ctx,30,1);
    if(nk_slider_float(&ctx,.75f,&settings->ui_scale,2.5f,.05f)) Save();
  }
  void Revert() {
    settings->width=previous_display.width;settings->height=previous_display.height;
    settings->window_mode=previous_display.window_mode;mode=settings->window_mode;
    std::string error;ApplyNativeDisplay(window,*settings,error);confirm_until=0;Save();
    auto it=std::find(resolutions.begin(),resolutions.end(),std::pair(settings->width,settings->height));
    resolution=it==resolutions.end()?0:int(it-resolutions.begin());status="Previous display mode restored.";
  }
  void Graphics() {
    Text("Output scaling");
    const char* filters[]={"Linear","Nearest neighbour","Sharpened"};
    nk_layout_row_dynamic(&ctx,36,1);
    const int filter=nk_combo(&ctx,filters,3,int(settings->filter),30,nk_vec2(300,160));
    if(filter!=int(settings->filter)) {settings->filter=xn::gfx::OutputFilter(filter);Apply();Save();}
    Text("Changes the final image at the chosen output size. The game's internal rendering size stays unchanged.",50);
    if(settings->filter==xn::gfx::OutputFilter::Sharpened) {
      Text("Sharpening strength");nk_layout_row_dynamic(&ctx,30,1);
      if(nk_slider_float(&ctx,0,&settings->sharpness,1,.05f)) {Apply();Save();}
    }
    Text("NVIDIA DLSS Super Resolution");
    nk_layout_row_dynamic(&ctx,36,1);nk_widget_disable_begin(&ctx);
    nk_button_label(&ctx,"Unavailable - temporal inputs not integrated");nk_widget_disable_end(&ctx);
    Text("DLSS needs depth, motion vectors and jitter from the renderer. The driver alone cannot enable it.",50);
    Text("NVIDIA frame generation (future)");
    nk_layout_row_dynamic(&ctx,36,1);nk_widget_disable_begin(&ctx);
    nk_button_label(&ctx,"Unavailable - no native frame-generation backend");nk_widget_disable_end(&ctx);
    Text("Reserved for a future supported integration. RTX 3060 does not support NVIDIA DLSS frame generation.",50);
    Text("Master volume");nk_layout_row_dynamic(&ctx,32,2);
    if(nk_slider_float(&ctx,0,&settings->volume,1,.05f)) {Apply();Save();}
    nk_label(&ctx,std::format("{}%",int(std::round(settings->volume*100))).c_str(),NK_TEXT_LEFT);
  }
  void Player() {
    Text("Local player name (1-15 characters)");nk_layout_row_dynamic(&ctx,36,2);
    nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,player_name.data(),player_name.size(),nk_filter_ascii);
    if(nk_button_label(&ctx,"Save player name")) {
      std::string error;
      status=xn::LocalUser().SetGamertag(player_name.data(),error)?"Player name saved. Profile identity and saves retained.":error;
    }
    Text("Used by the local profile and the next in-game name prompt. An existing driver name stored inside a save may need changing in-game.",54);
    if(!features.disc2) return;
    Text("Disc 2 folder (blank = detect in the game assets)");
    nk_layout_row_dynamic(&ctx,36,1);
    nk_edit_string_zero_terminated(&ctx,NK_EDIT_FIELD,disc_path.data(),disc_path.size(),nk_filter_default);
    nk_layout_row_dynamic(&ctx,36,2);
    if(nk_button_label(&ctx,"Browse folder...")) {
      // SDL may complete the native dialog on a worker thread. Only queue its
      // result here; widgets and settings remain owned by the SDL thread.
      auto state=dialog;
      {std::lock_guard lock(state->mutex);state->pending=true;}
      SDL_ShowOpenFolderDialog([](void* data,const char* const* paths,int) {
        std::unique_ptr<std::shared_ptr<DialogState>> holder(static_cast<std::shared_ptr<DialogState>*>(data));
        auto& state=**holder;std::lock_guard lock(state.mutex);
        if(paths && paths[0]) state.result=paths[0];
        state.pending=false;
      },new std::shared_ptr<DialogState>(state),window,disc_path[0]?disc_path.data():nullptr,false);
    }
    if(nk_button_label(&ctx,"Save Disc 2 path")) {settings->disc2_root=disc_path.data();if(Save()) status="Disc 2 path saved.";}
    nk_bool auto_install=settings->auto_install_disc2;
    nk_layout_row_dynamic(&ctx,32,1);nk_checkbox_label(&ctx,"Install / check Disc 2 at startup",&auto_install);
    if(bool(auto_install)!=settings->auto_install_disc2) {settings->auto_install_disc2=auto_install;Save();}
    nk_layout_row_dynamic(&ctx,36,1);
    if(installing) {nk_widget_disable_begin(&ctx);nk_button_label(&ctx,"Installing Disc 2...");nk_widget_disable_end(&ctx);}
    else if(nk_button_label(&ctx,"Check / install Disc 2 now")) StartInstall();
    std::string progress;{std::lock_guard lock(progress_mutex);progress=disc_status;}
    Text(progress,48);
    Text("Installs the four FM4 marketplace packages into native user data. After a manual install, restart the game to rescan content.",54);
  }
  struct DialogState {std::mutex mutex;std::string result;bool pending=false;int target=0;};
  std::shared_ptr<DialogState> dialog=std::make_shared<DialogState>();
  void StartInstall() {
    if(!features.disc2) return;
    if(installing.exchange(true)) return;
    if(installer.joinable()) installer.join();
    settings->disc2_root=disc_path.data();Save();
    const std::filesystem::path source=disc_path.data();
    installer=std::thread([this,source] {
      InstallNativeDisc2(source,game_root,save_root,[this](const std::string& progress) {
        std::lock_guard lock(progress_mutex);disc_status=progress;
      });
      installing=false;
    });
  }
  void Controls() {
    Text("F5 or Insert: open / close these settings");
    Text("Controller Back + Start: open / close settings");
    Text("Controller left stick / D-pad: move pointer; A: click; B: close");
    Text("Escape: close settings (outside settings it exits the game)");
    Text("F8: toggle the FPS counter");
    Text("F11 or Alt+Enter: toggle fullscreen");
    Text("Mouse and keyboard work throughout the menu. Game input is captured while settings are open.",50);
    Text("Settings do not pause the race automatically. Use Pause / resume below to send the game's Start button.",50);
    Text("Native profile: "+settings_path.string(),65);
    Text("Native saves / content: "+save_root.string(),65);
  }
  void Build() {
    int w=0,h=0,pw=0,ph=0;SDL_GetWindowSize(window,&w,&h);SDL_GetWindowSizeInPixels(window,&pw,&ph);
    if(!w || !h || !pw || !ph) return;
    // Keep all controls reachable even when a large scale is used in a small window.
    const float scale=std::min(settings->ui_scale,std::min(w/670.f,h/540.f));
    sx=float(pw)/w*scale;sy=float(ph)/h*scale;
    const float width=std::min(930.f,w/scale-24),height=std::min(720.f,h/scale-24);
    const auto bounds=nk_rect((w/scale-width)/2,(h/scale-height)/2,width,height);
    if(nk_begin(&ctx,features.title.c_str(),bounds,NK_WINDOW_BORDER|NK_WINDOW_TITLE|NK_WINDOW_NO_SCROLLBAR)) {
      if(features.setup_wizard) SetupWizard();
      else {
        nk_layout_row_dynamic(&ctx,35,1);
        nk_label(&ctx,std::format("{:.1f} FPS   |   {}   |   {} x {}",xn::MeasuredFrameRate(),renderer->DeviceName(),pw,ph).c_str(),NK_TEXT_LEFT);
        const int tab_count=features.player?4:3;
        nk_layout_row_dynamic(&ctx,38,tab_count);
        const char* tabs[]={"Display","Graphics / audio",features.disc2?"Player / Disc 2":"Player","Controls"};
        const int tab_map[]={0,1,features.player?2:3,3};
        for(int i=0;i<tab_count;++i) {
          const int actual=features.player?i:tab_map[i];
          if(nk_button_label(&ctx,tabs[actual])) tab=actual;
        }
        if(tab==2 && !features.player) tab=3;
        nk_layout_row_dynamic(&ctx,height-295,1);
        static constexpr const char* groups[]={"display-options","graphics-options","player-options","control-options"};
        if(nk_group_begin(&ctx,groups[tab],NK_WINDOW_BORDER)) {
          switch(tab) {case 0:Display();break;case 1:Graphics();break;case 2:Player();break;case 3:Controls();break;}
          nk_group_end(&ctx);
        }
        Text(status,44);
        nk_layout_row_dynamic(&ctx,36,3);
        if(nk_button_label(&ctx,"Save settings")) {
          if(features.disc2) settings->disc2_root=disc_path.data();
          if(Save()) status="Native settings saved.";
        }
        if(nk_button_label(&ctx,"Pause / resume game")) xn::SendHostStartButton();
        if(nk_button_label(&ctx,"Close (F5 / Esc)")) Open(false);
      }
    }
    nk_end(&ctx);
    if(!open) {nk_clear(&ctx);return;}
    nk_buffer commands,vertices,indices;
    nk_buffer_init_default(&commands);nk_buffer_init_default(&vertices);nk_buffer_init_default(&indices);
    const nk_draw_vertex_layout_element layout[]={
      {NK_VERTEX_POSITION,NK_FORMAT_FLOAT,offsetof(xn::gfx::HostUiVertex,position)},
      {NK_VERTEX_TEXCOORD,NK_FORMAT_FLOAT,offsetof(xn::gfx::HostUiVertex,uv)},
      {NK_VERTEX_COLOR,NK_FORMAT_R8G8B8A8,offsetof(xn::gfx::HostUiVertex,color)},
      {NK_VERTEX_LAYOUT_END}};
    nk_convert_config config{};config.vertex_layout=layout;
    config.vertex_size=sizeof(xn::gfx::HostUiVertex);config.vertex_alignment=alignof(xn::gfx::HostUiVertex);
    config.tex_null=null_texture;config.circle_segment_count=config.curve_segment_count=config.arc_segment_count=22;
    config.global_alpha=1;config.shape_AA=config.line_AA=NK_ANTI_ALIASING_ON;
    if(nk_convert(&ctx,&commands,&vertices,&indices,&config)==NK_CONVERT_SUCCESS) {
      auto frame=std::make_shared<xn::gfx::HostUiFrame>();frame->font=font;
      auto* v=static_cast<const xn::gfx::HostUiVertex*>(nk_buffer_memory_const(&vertices));
      frame->vertices.assign(v,v+vertices.allocated/sizeof(*v));
      for(auto& vertex:frame->vertices) {vertex.position[0]*=sx;vertex.position[1]*=sy;}
      auto* idx=static_cast<const uint32_t*>(nk_buffer_memory_const(&indices));
      frame->indices.assign(idx,idx+indices.allocated/sizeof(*idx));
      const nk_draw_command* command;uint32_t offset=0;
      nk_draw_foreach(command,&ctx,&commands) {
        frame->commands.push_back({{command->clip_rect.x*sx,command->clip_rect.y*sy,
            command->clip_rect.w*sx,command->clip_rect.h*sy},offset,command->elem_count});
        offset+=command->elem_count;
      }
      renderer->SetHostOverlay(std::move(frame));
    }
    nk_buffer_free(&commands);nk_buffer_free(&vertices);nk_buffer_free(&indices);nk_clear(&ctx);
  }
};
NativeOptions::NativeOptions(NativeOptionsConfig config):impl_(std::make_unique<Impl>()) {
  impl_->features=std::move(config);
}
NativeOptions::~NativeOptions()=default;
bool NativeOptions::Initialize(SDL_Window* window,xn::gfx::VulkanRenderer* renderer,NativeSettings& settings,
    const std::filesystem::path& path,const std::filesystem::path& game,const std::filesystem::path& saves,std::string& error) {
  auto& im=*impl_;im.window=window;im.renderer=renderer;im.settings=&settings;im.settings_path=path;im.game_root=game;im.save_root=saves;
  nk_font_atlas_init_default(&im.atlas);nk_font_atlas_begin(&im.atlas);
  auto* face=nk_font_atlas_add_from_file(&im.atlas,XN_NATIVE_FONT,20,nullptr);
  if(!face) face=nk_font_atlas_add_default(&im.atlas,20,nullptr);
  int w=0,h=0;const void* pixels=nk_font_atlas_bake(&im.atlas,&w,&h,NK_FONT_ATLAS_RGBA32);
  if(!face || !pixels || w<=0 || h<=0) {error="Could not build the Nuklear font atlas.";nk_font_atlas_clear(&im.atlas);return false;}
  im.font=std::make_shared<xn::gfx::HostUiFont>();im.font->width=w;im.font->height=h;
  im.font->rgba.assign(static_cast<const uint8_t*>(pixels),static_cast<const uint8_t*>(pixels)+w*h*4);
  nk_font_atlas_end(&im.atlas,nk_handle_id(1),&im.null_texture);
  if(!nk_init_default(&im.ctx,&face->handle)) {error="Could not initialize Nuklear.";nk_font_atlas_clear(&im.atlas);return false;}
  im.initialized=true;
  std::array<nk_color,NK_COLOR_COUNT> colors;
  colors.fill(nk_rgb(52,55,62));
  colors[NK_COLOR_TEXT]=nk_rgb(240,242,245);colors[NK_COLOR_WINDOW]=nk_rgba(19,22,28,248);
  colors[NK_COLOR_HEADER]=nk_rgb(151,24,39);colors[NK_COLOR_BORDER]=nk_rgb(70,75,85);
  colors[NK_COLOR_BUTTON]=nk_rgb(49,55,65);colors[NK_COLOR_BUTTON_HOVER]=nk_rgb(150,36,50);
  colors[NK_COLOR_BUTTON_ACTIVE]=nk_rgb(191,38,56);colors[NK_COLOR_EDIT]=nk_rgb(29,32,40);
  colors[NK_COLOR_EDIT_CURSOR]=nk_rgb(255,255,255);colors[NK_COLOR_TOGGLE_CURSOR]=nk_rgb(225,55,68);
  colors[NK_COLOR_SLIDER_CURSOR]=nk_rgb(218,50,65);colors[NK_COLOR_SLIDER_CURSOR_HOVER]=nk_rgb(245,75,88);
  colors[NK_COLOR_SLIDER_CURSOR_ACTIVE]=nk_rgb(255,95,105);nk_style_from_table(&im.ctx,colors.data());
  im.ctx.style.scrollv.normal=im.ctx.style.scrollh.normal=nk_style_item_color(nk_rgb(26,29,36));
  im.ctx.style.scrollv.cursor_normal=im.ctx.style.scrollh.cursor_normal=nk_style_item_color(nk_rgb(106,112,126));
  im.ctx.style.scrollv.cursor_hover=im.ctx.style.scrollh.cursor_hover=nk_style_item_color(nk_rgb(149,156,169));
  im.ctx.style.window.padding=nk_vec2(14,12);im.ctx.style.window.spacing=nk_vec2(10,7);
  im.ctx.clip.userdata=nk_handle_ptr(nullptr);
  im.ctx.clip.copy=[](nk_handle,const char* text,int length){const std::string s(text,length);SDL_SetClipboardText(s.c_str());};
  im.ctx.clip.paste=[](nk_handle,nk_text_edit* edit){char* text=SDL_GetClipboardText();if(text){nk_textedit_paste(edit,text,std::strlen(text));SDL_free(text);}};
  im.resolutions={{1280,720},{1600,900},{1920,1080},{2560,1440},{3440,1440},{3840,2160},{5120,1440}};
  int count=0;auto** modes=SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window),&count);
  for(int i=0;i<count;++i) if(modes[i]->w>=640 && modes[i]->h>=360 && modes[i]->w<=16384 && modes[i]->h<=8640)
    im.resolutions.emplace_back(modes[i]->w,modes[i]->h);
  SDL_free(modes);im.resolutions.emplace_back(settings.width,settings.height);
  std::sort(im.resolutions.begin(),im.resolutions.end());im.resolutions.erase(std::unique(im.resolutions.begin(),im.resolutions.end()),im.resolutions.end());
  std::snprintf(im.disc_path.data(),im.disc_path.size(),"%s",settings.disc2_root.c_str());
  if(!im.features.setup_wizard)
    std::snprintf(im.player_name.data(),im.player_name.size(),"%s",xn::LocalUser().gamertag().c_str());
  im.initial_display_pending=settings.window_mode==0 && std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  im.Apply();nk_input_begin(&im.ctx);return true;
}
void NativeOptions::SetOpen(bool open){impl_->Open(open);}
bool NativeOptions::IsOpen()const{return impl_->open;}
bool NativeOptions::InstallingDisc2()const{return impl_->installing;}
void NativeOptions::InstallDisc2(){if(impl_->features.disc2){impl_->tab=2;impl_->StartInstall();}}
bool NativeOptions::HandleEvent(const SDL_Event& e) {
  auto& im=*impl_;if(!im.initialized) return false;
  if(im.features.setup_wizard) {
    if(e.type==SDL_EVENT_QUIT || e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key==SDLK_ESCAPE) ||
        (e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button==SDL_GAMEPAD_BUTTON_EAST)) {
      if(im.setup_page!=Impl::Installing || im.setup_finished) im.setup_cancelled=true;
      return true;
    }
  }
  if(!im.features.setup_wizard && e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat &&
      (e.key.key==SDLK_F11 || (e.key.key==SDLK_RETURN && (e.key.mod&SDL_KMOD_ALT)))) {
    const auto previous=*im.settings;
    im.settings->window_mode=(SDL_GetWindowFlags(im.window)&SDL_WINDOW_FULLSCREEN)?0:1;
    std::string error;
    if(!ApplyNativeDisplay(im.window,*im.settings,error)) {*im.settings=previous;im.status=error;}
    else {im.mode=im.settings->window_mode;im.Save();}
    return true;
  }
  if(!im.features.setup_wizard && e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.key==SDLK_F5 || e.key.key==SDLK_INSERT)) {im.Open(!im.open);return true;}
  if(!im.features.setup_wizard && e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key==SDLK_F8) {
    im.settings->show_fps=!im.settings->show_fps;im.Apply();im.Save();return true;
  }
  if(!im.features.setup_wizard && e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && (e.gbutton.button==SDL_GAMEPAD_BUTTON_BACK || e.gbutton.button==SDL_GAMEPAD_BUTTON_START)) {
    auto* pad=SDL_GetGamepadFromID(e.gbutton.which);
    if(pad && SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_START)) {im.Open(!im.open);return true;}
  }
  if(!im.open) return false;
  if(e.type==SDL_EVENT_KEY_DOWN || e.type==SDL_EVENT_KEY_UP) {
    const bool down=e.type==SDL_EVENT_KEY_DOWN;
    if(down && e.key.key==SDLK_ESCAPE) {im.Open(false);return true;}
    nk_keys key=NK_KEY_NONE;
    switch(e.key.key) {
      case SDLK_LSHIFT:case SDLK_RSHIFT:key=NK_KEY_SHIFT;break;
      case SDLK_DELETE:key=NK_KEY_DEL;break;case SDLK_RETURN:key=NK_KEY_ENTER;break;
      case SDLK_TAB:key=NK_KEY_TAB;break;case SDLK_BACKSPACE:key=NK_KEY_BACKSPACE;break;
      case SDLK_LEFT:key=NK_KEY_LEFT;break;case SDLK_RIGHT:key=NK_KEY_RIGHT;break;
      case SDLK_UP:key=NK_KEY_UP;break;case SDLK_DOWN:key=NK_KEY_DOWN;break;
      case SDLK_HOME:key=NK_KEY_TEXT_START;break;case SDLK_END:key=NK_KEY_TEXT_END;break;
      case SDLK_PAGEUP:key=NK_KEY_SCROLL_UP;break;case SDLK_PAGEDOWN:key=NK_KEY_SCROLL_DOWN;break;
      case SDLK_C:if(!down || (e.key.mod&SDL_KMOD_CTRL)) key=NK_KEY_COPY;break;
      case SDLK_V:if(!down || (e.key.mod&SDL_KMOD_CTRL)) key=NK_KEY_PASTE;break;
      case SDLK_X:if(!down || (e.key.mod&SDL_KMOD_CTRL)) key=NK_KEY_CUT;break;
      case SDLK_A:if(!down || (e.key.mod&SDL_KMOD_CTRL)) key=NK_KEY_TEXT_SELECT_ALL;break;
      default:break;
    }
    if(key!=NK_KEY_NONE) nk_input_key(&im.ctx,key,down);
    return true;
  }
  float mouse_x=0,mouse_y=0;SDL_GetMouseState(&mouse_x,&mouse_y);
  const float scale=im.settings->ui_scale;
  int w=0,h=0;SDL_GetWindowSize(im.window,&w,&h);
  const float actual=std::min(scale,std::min(w/670.f,h/540.f));
  if(e.type==SDL_EVENT_MOUSE_MOTION) nk_input_motion(&im.ctx,e.motion.x/actual,e.motion.y/actual);
  else if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN || e.type==SDL_EVENT_MOUSE_BUTTON_UP) {
    nk_input_motion(&im.ctx,e.button.x/actual,e.button.y/actual);
    if(std::getenv("XN_NATIVE_MENU_INPUT_LOG")) std::fprintf(stderr,"native-menu mouse %s %.1f %.1f scale %.2f\n",
        e.type==SDL_EVENT_MOUSE_BUTTON_DOWN?"down":"up",e.button.x,e.button.y,actual);
    const auto button=e.button.button==SDL_BUTTON_LEFT?NK_BUTTON_LEFT:e.button.button==SDL_BUTTON_RIGHT?NK_BUTTON_RIGHT:NK_BUTTON_MIDDLE;
    nk_input_button(&im.ctx,button,e.button.x/actual,e.button.y/actual,e.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
  } else if(e.type==SDL_EVENT_MOUSE_WHEEL) nk_input_scroll(&im.ctx,nk_vec2(e.wheel.x,e.wheel.y));
  else if(e.type==SDL_EVENT_TEXT_INPUT) {
    const char* text=e.text.text;
    while(*text) {nk_rune rune;const int n=nk_utf_decode(text,&rune,std::strlen(text));if(n<=0) break;nk_input_unicode(&im.ctx,rune);text+=n;}
  } else if(e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button==SDL_GAMEPAD_BUTTON_EAST) im.Open(false);
  return e.type!=SDL_EVENT_QUIT && e.type!=SDL_EVENT_WINDOW_CLOSE_REQUESTED;
}
void NativeOptions::Update() {
  auto& im=*impl_;if(!im.initialized) return;
  if(im.initial_display_pending && im.renderer->PresentedFrames()) {
    if(ResizeHyprlandWindow(im.settings->width,im.settings->height) || ++im.initial_display_attempts>=30)
      im.initial_display_pending=false;
  }
  const auto now=SDL_GetTicks();
  if(im.confirm_until && now>=im.confirm_until) im.Revert();
  {std::lock_guard lock(im.dialog->mutex);if(!im.dialog->result.empty()) {
    auto* target=im.dialog->target==1?im.disc1_source.data():im.dialog->target==2?im.disc2_source.data():
        im.dialog->target==3?im.local_files_root.data():im.dialog->target==4?im.save_games_root.data():im.disc_path.data();
    const size_t target_size=im.dialog->target==1?im.disc1_source.size():im.dialog->target==2?im.disc2_source.size():
        im.dialog->target==3?im.local_files_root.size():im.dialog->target==4?im.save_games_root.size():im.disc_path.size();
    std::snprintf(target,target_size,"%s",im.dialog->result.c_str());im.dialog->result.clear();
  }}
  if(im.features.setup_wizard && im.setup_finished && im.setup_page==Impl::Installing && im.setup_result.ok) {
    if(im.setup_worker.joinable()) im.setup_worker.join();
    im.setup_receipt=im.setup_result.receipt;
    im.setup_page=Impl::Done;
  }
  if(!im.open || now-im.last_update<16) return;
  const float dt=std::min(.05f,(now-im.last_update)/1000.f);im.last_update=now;
  if(im.pad && SDL_GamepadConnected(im.pad)) {
    auto axis=[&](SDL_GamepadAxis a) {float v=SDL_GetGamepadAxis(im.pad,a)/32767.f;return std::abs(v)<.2f?0.f:v;};
    float dx=axis(SDL_GAMEPAD_AXIS_LEFTX),dy=axis(SDL_GAMEPAD_AXIS_LEFTY);
    dx+=SDL_GetGamepadButton(im.pad,SDL_GAMEPAD_BUTTON_DPAD_RIGHT)-SDL_GetGamepadButton(im.pad,SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    dy+=SDL_GetGamepadButton(im.pad,SDL_GAMEPAD_BUTTON_DPAD_DOWN)-SDL_GetGamepadButton(im.pad,SDL_GAMEPAD_BUTTON_DPAD_UP);
    const bool pressed=SDL_GetGamepadButton(im.pad,SDL_GAMEPAD_BUTTON_SOUTH);
    if(dx || dy || pressed!=im.pad_mouse) {
      int w=0,h=0;SDL_GetWindowSize(im.window,&w,&h);
      float x=0,y=0;SDL_GetMouseState(&x,&y);x=std::clamp(x+dx*650*dt,0.f,float(w-1));y=std::clamp(y+dy*650*dt,0.f,float(h-1));
      SDL_WarpMouseInWindow(im.window,x,y);
      const float scale=std::min(im.settings->ui_scale,std::min(w/670.f,h/540.f));
      nk_input_motion(&im.ctx,x/scale,y/scale);nk_input_button(&im.ctx,NK_BUTTON_LEFT,x/scale,y/scale,pressed);im.pad_mouse=pressed;
    }
  }
  nk_input_end(&im.ctx);
  if(std::getenv("XN_NATIVE_MENU_INPUT_LOG") && im.ctx.input.mouse.buttons[NK_BUTTON_LEFT].clicked)
    std::fprintf(stderr,"native-menu button clicked=%u down=%d position=%.1f,%.1f tab=%d\n",
        im.ctx.input.mouse.buttons[NK_BUTTON_LEFT].clicked,im.ctx.input.mouse.buttons[NK_BUTTON_LEFT].down,
        im.ctx.input.mouse.buttons[NK_BUTTON_LEFT].clicked_pos.x,im.ctx.input.mouse.buttons[NK_BUTTON_LEFT].clicked_pos.y,im.tab);
  im.Build();nk_input_begin(&im.ctx);
  if(im.open) im.renderer->RedrawHostOverlay();
}
bool RunForzaSetupWizard(SDL_Window* window,xn::gfx::VulkanRenderer* renderer,
    NativeSettings& settings,const ForzaSetupPaths& paths,ForzaSetupReceipt& receipt,
    std::string& error,double seconds_limit) {
  error.clear();
  if(!window || !renderer) {error="Setup needs a display and Vulkan renderer.";return false;}
  NativeOptionsConfig config;
  config.title="FORZA MOTORSPORT 4 | FIRST RUN SETUP";
  config.disc2=false;config.player=false;config.setup_wizard=true;
  NativeOptions menu(std::move(config));
  auto& im=*menu.impl_;
  im.setup_paths=paths;
  std::error_code source_error;
  std::filesystem::path initial_source=paths.current_game_root;
  if(!receipt.source_reference.empty() &&
      std::filesystem::exists(receipt.source_reference,source_error))
    initial_source=receipt.source_reference;
  else if(!receipt.game_root.empty() && std::filesystem::is_directory(receipt.game_root,source_error))
    initial_source=receipt.game_root;
  std::snprintf(im.disc1_source.data(),im.disc1_source.size(),"%s",initial_source.string().c_str());
  std::snprintf(im.disc2_source.data(),im.disc2_source.size(),"%s",settings.disc2_root.c_str());
  const auto local_root=receipt.game_root.filename()=="game"?
      receipt.game_root.parent_path():paths.profile_root/"files/forza4";
  const auto save_root=receipt.save_root.empty()?paths.save_root:receipt.save_root;
  std::snprintf(im.local_files_root.data(),im.local_files_root.size(),"%s",local_root.string().c_str());
  std::snprintf(im.save_games_root.data(),im.save_games_root.size(),"%s",save_root.string().c_str());
  const std::string player=ForzaSetupPlayer(paths.profile_root,error);
  if(!error.empty()) return false;
  std::snprintf(im.player_name.data(),im.player_name.size(),"%s",player.c_str());
  if(!menu.Initialize(window,renderer,settings,paths.profile_root/"forza4-native.toml",
      paths.current_game_root,paths.save_root,error)) return false;
  menu.SetOpen(true);
  const auto started=SDL_GetTicks();
  while(!im.setup_done && !im.setup_cancelled) {
    SDL_Event e;
    while(SDL_PollEvent(&e)) {
      if(!menu.HandleEvent(e)) xn::HandleHostWindowEvent(window,e);
    }
    renderer->Present();
    menu.Update();
    if(seconds_limit>0 && double(SDL_GetTicks()-started)/1000.0>=seconds_limit &&
        (im.setup_page!=NativeOptions::Impl::Installing || im.setup_finished)) break;
    SDL_Delay(4);
  }
  if(im.setup_worker.joinable()) im.setup_worker.join();
  if(im.setup_done) receipt=im.setup_receipt;
  menu.SetOpen(false);
  return im.setup_done;
}
}
