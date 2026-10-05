// bo1 - port options and runtime defaults

#include "bo1_settings.h"

#include <array>
#include <fstream>
#include <utility>

#include <rex/logging.h>

// Empty = Windows user name (filled in at startup if there is no saved value).
REXCVAR_DEFINE_STRING(bo1_gamertag, "", "Black Ops",
                      "Player name the game sees (at most 15 ASCII characters)");

REXCVAR_DEFINE_INT32(bo1_backup_count, 10, "Black Ops",
                     "Profile and save backups to keep (0 = none)")
    .range(0, 100);

REXCVAR_DEFINE_BOOL(bo1_dev_mode, true, "Black Ops",
                    "Developer mode: extended engine information in the developer console (F1) "
                    "and runs bo1_dev_commands in the engine at startup");

// Empty by default: the engine's own developer modes change how the game plays. "developer 1"
// turns developer-only checks into errors (e.g. a drop to the menu with "VEH_GetSeat(): Entity not
// a vehicle" in the campaign) and "developer_script 1" compiles the script asserts, which fail
// with the retail data ("server script runtime error ... assert fail" in multiplayer matches).
REXCVAR_DEFINE_STRING(bo1_dev_commands, "", "Black Ops",
                      "Engine commands run at startup in developer mode, separated by ';'. Leave "
                      "empty to play: 'developer 1' and 'developer_script 1' make the retail game "
                      "fail on developer checks and script asserts");

REXCVAR_DEFINE_BOOL(bo1_console, false, "Black Ops",
                    "Also open the log in a separate Windows console window (the in-game "
                    "developer console, F1, is always available)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_console_level, "info", "Black Ops",
                      "Minimum level of the messages shown by the developer consoles")
    .allowed({"trace", "debug", "info", "warn", "error"});

REXCVAR_DEFINE_BOOL(bo1_overlay, true, "Black Ops",
                    "Compact in-game overlay (F2 shows or hides it): FPS, frame time, files "
                    "being loaded and latest warnings");

REXCVAR_DEFINE_BOOL(bo1_profiler, false, "Black Ops",
                    "Tool: starts the Tracy profiler (connect with tracy-profiler or "
                    "tracy-capture); marks every game frame")
    .debug_only();

REXCVAR_DEFINE_STRING(bo1_dump_xex, "", "Black Ops",
                      "Tool: dumps the loaded XEX (with the TU applied) to this path and closes "
                      "the game before running it")
    .debug_only();

REXCVAR_DEFINE_STRING(bo1_capture_at, "", "Black Ops",
                      "Tests: seconds after startup at which to save a screenshot of the game "
                      "image to the logs folder (e.g. \"40,65\")")
    .debug_only();

REXCVAR_DEFINE_STRING(bo1_capture_prefix, "capture", "Black Ops",
                      "Tests: file name prefix of the bo1_capture_at screenshots")
    .debug_only();

REXCVAR_DEFINE_BOOL(bo1_test_ignore_input, false, "Black Ops",
                    "Tests: the game receives no controller, keyboard or mouse input")
    .debug_only();

REXCVAR_DEFINE_STRING(bo1_test_exec, "", "Black Ops",
                      "Tests: developer console lines (separated by ';'; \"wait N\" pauses N "
                      "seconds) run once, bo1_test_exec_delay seconds after the first frame")
    .debug_only();

REXCVAR_DEFINE_INT32(bo1_test_exec_delay, 15, "Black Ops",
                     "Tests: seconds to wait before running bo1_test_exec")
    .range(0, 600)
    .debug_only();

namespace bo1 {

namespace {

// Options read before the GPU plugin is loaded: written to a new bo1.toml, in TOML syntax.
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kConfigDefaults{{
    {"log_level", "\"info\""},
    {"gpu_plugin", "\"xenos\""},
    // Always windowed.
    {"fullscreen", "false"},
}};

// GPU options with a different default in the port. They only exist once the plugin is loaded
// and are applied when neither bo1.toml nor the command line sets them.
// (Antialiasing at presentation is set by bo1_antialiasing, see bo1_graphics.cpp.)
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kGpuDefaults{{
    // The game already caps itself at 60 FPS with its own vsync (r_vsync, like the console). The
    // PC vsync added waits to presentation: in a match it dropped to 53-57 FPS. In a window
    // Windows composes the image, so there is no tearing.
    {"vsync", "false"},
    // The runtime uploaded again, at the end of every frame, all the memory the CPU had already
    // uploaded (~29 MB per frame in a match). Page protection already keeps coherency: every CPU
    // write invalidates only what changes.
    {"clear_memory_page_state", "false"},
    // Copying what memexport shaders write back to the CPU forced a wait for the GPU in the
    // middle of the frame (the end of match screen dropped to ~25 FPS). This game does not need
    // it: the data stays in GPU memory, where the following draws use it.
    {"readback_memexport", "false"},
}};

std::string TomlString(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

void WriteDefaultConfigIfMissing(const std::filesystem::path& config_path,
                                 std::string_view default_game_root) {
  if (std::filesystem::exists(config_path)) {
    return;
  }
  std::ofstream out(config_path);
  if (!out) {
    REXLOG_WARN("bo1: could not create {}", config_path.string());
    return;
  }
  out << "# Call of Duty: Black Ops (recompiled) configuration\n";
  if (!default_game_root.empty()) {
    out << "game_data_root = " << TomlString(default_game_root) << "\n";
  }
  for (const auto& [name, value] : kConfigDefaults) {
    out << name << " = " << value << "\n";
  }
  REXLOG_INFO("bo1: created {} with the port defaults", config_path.string());
}

void ApplyGpuDefaults() {
  for (const auto& [name, value] : kGpuDefaults) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
    }
  }
}

}  // namespace bo1
