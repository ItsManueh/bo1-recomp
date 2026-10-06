// bo1 - application: paths, configuration, debugging and port services
//
// The campaign (bo1) and multiplayer (bo1mp) share this code; each main.cpp sets the executable
// name, the XEX and the window title.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <rex/input/input_system.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "bo1_audio.h"
#include "bo1_capture.h"
#include "bo1_debug.h"
#include "bo1_engine.h"
#include "bo1_framerate.h"
#include "bo1_graphics.h"
#include "bo1_kernel.h"
#include "bo1_players.h"
#include "bo1_settings.h"
#include "bo1_storage.h"
#include "bo1_tu.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#ifndef BO1_DEFAULT_GAME_ROOT
#define BO1_DEFAULT_GAME_ROOT ""
#endif
#ifndef BO1_DEFAULT_UPDATE_ROOT
#define BO1_DEFAULT_UPDATE_ROOT ""
#endif
#ifndef BO1_APP_NAME
#define BO1_APP_NAME "bo1"
#endif
#ifndef BO1_XEX_NAME
#define BO1_XEX_NAME "default.xex"
#endif
#ifndef BO1_WINDOW_TITLE
#define BO1_WINDOW_TITLE "Call of Duty: Black Ops (recompiled)"
#endif

class Bo1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo1App>(new Bo1App(ctx, BO1_APP_NAME, PPCImageConfig));
  }

 protected:
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = std::string("game:\\") + BO1_XEX_NAME;
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_ = paths.config_path;
    bo1::WriteDefaultConfigIfMissing(paths.config_path, BO1_DEFAULT_GAME_ROOT);
    // ReXApp computes the paths before reading bo1.toml: it is read here so the saved paths count
    // (the command line still has priority).
    if (std::filesystem::exists(paths.config_path)) {
      rex::cvar::LoadConfig(paths.config_path);
    }
    if (paths.game_data_root.empty()) {
      std::string root = rex::cvar::Query<std::string>("game_data_root");
      paths.game_data_root = root.empty() ? std::string(BO1_DEFAULT_GAME_ROOT) : root;
    }
    // Title Update data (patch*.ff, img_patch.pak...): the game reads it from "update:".
    if (paths.update_data_root.empty()) {
      std::string root = rex::cvar::Query<std::string>("update_data_root");
      if (root.empty()) root = BO1_DEFAULT_UPDATE_ROOT;
      if (!root.empty() && std::filesystem::is_directory(root)) paths.update_data_root = root;
    }
    // Profile and saves in Windows "Saved Games", unless user_data_root is set.
    if (rex::cvar::Query<std::string>("user_data_root").empty()) {
      auto saved_games = bo1::DefaultUserDataRoot();
      if (!saved_games.empty()) {
        paths.user_data_root = saved_games;
        paths.cache_root = saved_games / "cache";
      }
    }
    user_root_ = paths.user_data_root;
    bo1::SetLaunchDataFile(user_root_ / "launch_data.bin");
    if (REXCVAR_GET(bo1_gamertag).empty()) {
      rex::cvar::SetFlagByName("bo1_gamertag", bo1::WindowsUserName());
    }
  }

  void OnPostInitLogging() override {
    // 1 ms Windows timer resolution. Without it, every Sleep(1) lasts ~15.6 ms on Windows 11: the
    // emulated console vblank (a thread that sleeps 1 ms between checks) arrived in bursts and the
    // game's short waits got longer, dropping frames.
    timeBeginPeriod(1);
    if (REXCVAR_GET(bo1_profiler)) rex::perf::Profiler::Startup();
    bo1::debug::Initialize();
    if (REXCVAR_GET(bo1_console)) bo1::debug::OpenWindowsConsole(BO1_WINDOW_TITLE " - log");
  }

  // GPU options are registered when the plugin DLL is loaded, which ReXApp does after reading
  // bo1.toml: it is loaded here and the file read again so they count from the first frame.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.graphics || config.gpu_plugin.empty()) return;
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "d3d12");
    if (config.graphics && std::filesystem::exists(config_path_)) {
      rex::cvar::LoadConfig(config_path_);
    }
    bo1::ApplyGpuDefaults();
    bo1::graphics::ApplyRuntimeOptions();
    bo1::framerate::ApplyRuntimeOptions();
    bo1::players::ApplyRuntimeOptions();
    bo1::audio::ApplyRuntimeOptions();
  }

  void OnPostLoadXexImage() override {
    // Tool mode (--bo1_dump_xex=path): dump the executable already patched by the TU and exit
    // before a single game instruction runs.
    std::string dump = REXCVAR_GET(bo1_dump_xex);
    if (!dump.empty()) {
      bool ok = bo1::DumpLoadedXex(runtime()->kernel_state(), dump);
      rex::FlushLogging();
      TerminateProcess(GetCurrentProcess(), ok ? 0 : 1);
    }
    // The translated code belongs to Title Update #11. If another version is loaded (e.g. the
    // .xexp next to the .xex is missing), code and data do not match: stop with a clear message.
    uint32_t loaded = bo1::LoadedImageSize(runtime()->kernel_state());
    if (loaded != PPCImageConfig.image_size) {
      std::string xex = BO1_XEX_NAME;
      std::string msg = fmt::format(
          "The loaded game version does not match the recompiled one.\n\n"
          "Loaded image: {:#x} bytes, expected: {:#x} bytes.\n\n"
          "This executable was built for Title Update #11: check that {} is in the game "
          "folder next to {}.",
          loaded, PPCImageConfig.image_size, xex.substr(0, xex.size() - 4) + ".xexp", xex);
      REXLOG_ERROR("bo1: {}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      rex::FlushLogging();
      TerminateProcess(GetCurrentProcess(), 2);
    }
    REXLOG_INFO("bo1: loaded image matches the recompiled one ({:#x} bytes)", loaded);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    window()->SetTitle(BO1_WINDOW_TITLE);
    auto graphics_provider = [this]() -> rex::ui::GraphicsProvider* {
      auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
      return graphics ? graphics->provider() : nullptr;
    };
    ui_ = bo1::debug::CreateUi(drawer, BO1_WINDOW_TITLE, &app_context(), graphics_provider);
    rex::ui::RegisterBind("bind_bo1_console", "F1", "Developer console",
                          [] { bo1::debug::ToggleConsole(); });
    rex::ui::RegisterBind("bind_bo1_overlay", "F2", "Compact debug overlay",
                          [] { bo1::debug::ToggleOverlay(); });
    // Borderless fullscreen at the monitor's own resolution (ReXApp applies the option).
    rex::ui::RegisterBind("bind_bo1_fullscreen", "F11", "Toggle fullscreen", [] {
      const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
      rex::cvar::SetFlagByName("fullscreen", fullscreen ? "false" : "true");
    });
    rex::ui::RegisterBind("bind_bo1_screenshot", "F12", "Screenshot of the game image", [this] {
      SYSTEMTIME t;
      GetLocalTime(&t);
      std::string name = fmt::format("BlackOps_{:04}{:02}{:02}_{:02}{:02}{:02}", t.wYear, t.wMonth,
                                     t.wDay, t.wHour, t.wMinute, t.wSecond);
      bo1::SaveGameScreenshot(runtime(), user_root_ / "Screenshots", name);
    });
  }

  void OnPostSetup() override {
    bo1::engine::Initialize(runtime()->memory());
    REXLOG_INFO("bo1: profile and saves in {} (player '{}')", user_root_.string(),
                REXCVAR_GET(bo1_gamertag));
    bo1::BackupUserData(user_root_, REXCVAR_GET(bo1_backup_count));
    bo1::MountCachePartition(runtime()->file_system(), user_root_ / "cache_xbox");
    if (int n = bo1::InstallPendingDlc(runtime()->kernel_state(), user_root_ / "DLC")) {
      REXLOG_INFO("bo1: {} new DLC installed", n);
    }
    for (const auto& dlc : bo1::ListInstalledDlc(runtime()->kernel_state())) {
      REXLOG_INFO("bo1: DLC available: {}", dlc.file_name);
    }
    bo1::StartTimedCaptures(runtime(), config_path_.parent_path() / "logs");
    // The game gets no input while the developer console has the keyboard (and never in
    // automated tests).
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      const bool ignore_input = REXCVAR_GET(bo1_test_ignore_input);
      input->SetActiveCallback(
          [ignore_input] { return !ignore_input && !bo1::debug::ConsoleOpen(); });
      bo1::players::AddTestPads(input);
    }
  }

  void OnShutdown() override {
    timeEndPeriod(1);
    bo1::StopTimedCaptures();
    bo1::debug::DestroyConsoleWindow();
    ui_.reset();
  }

 private:
  std::filesystem::path config_path_;
  std::filesystem::path user_root_;
  std::unique_ptr<rex::ui::ImGuiDialog> ui_;
};
