// bo1 - port options ("Black Ops" category in bo1.toml)

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <rex/cvar.h>

// Profile and saves.
REXCVAR_DECLARE(std::string, bo1_gamertag);
REXCVAR_DECLARE(int32_t, bo1_backup_count);

// Debugging and developer mode.
REXCVAR_DECLARE(bool, bo1_dev_mode);
REXCVAR_DECLARE(std::string, bo1_dev_commands);
REXCVAR_DECLARE(bool, bo1_console);
REXCVAR_DECLARE(std::string, bo1_console_level);
REXCVAR_DECLARE(bool, bo1_overlay);

// Tools and automated tests (command line only).
REXCVAR_DECLARE(bool, bo1_profiler);
REXCVAR_DECLARE(std::string, bo1_dump_xex);
REXCVAR_DECLARE(std::string, bo1_capture_at);
REXCVAR_DECLARE(std::string, bo1_capture_prefix);
REXCVAR_DECLARE(bool, bo1_test_ignore_input);
REXCVAR_DECLARE(std::string, bo1_test_exec);
REXCVAR_DECLARE(int32_t, bo1_test_exec_delay);

namespace bo1 {

// Writes bo1.toml with the port defaults if it does not exist yet.
void WriteDefaultConfigIfMissing(const std::filesystem::path& config_path,
                                 std::string_view default_game_root);

// Port defaults for GPU plugin options that neither bo1.toml nor the command line set. Call after
// the plugin has been loaded.
void ApplyGpuDefaults();

}  // namespace bo1
