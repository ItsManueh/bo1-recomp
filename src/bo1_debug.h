// bo1 - debugging system: log capture, frame timing, load tracking, thread usage and the
// developer console commands. The in-game UI lives in bo1_debug_ui.cpp.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace bo1::debug {

// --- Setup ---------------------------------------------------------------------------------------
// Installs the log sinks (in-game console buffer and warnings). Call once logging is up.
void Initialize();
// Optional separate Windows console window with the colored log and a command line.
void OpenWindowsConsole(const char* title);

// --- Hooks from the rest of the port -------------------------------------------------------------
void OnGameFrame();  // once per game frame, from Com_Frame
void OnFileOpened(uint32_t handle, const std::string& path);
void OnFileRead(uint32_t handle, uint64_t bytes);
void OnFileClosed(uint32_t handle);
// Port message for the overlay and the log (e.g. engine errors).
void Notice(const std::string& text);

// --- Commands ------------------------------------------------------------------------------------
// Runs a developer console line: built-in commands (see "help") or, otherwise, an engine command.
void RunCommand(const std::string& line);
// Names of the built-in commands, for completion.
const std::vector<std::string>& BuiltinCommands();

// --- Snapshots for the UI --------------------------------------------------------------------------
enum class Level : uint8_t { kTrace, kDebug, kInfo, kWarn, kError };

struct LogLine {
  uint64_t sequence;
  std::string time;      // "HH:MM:SS.mmm"
  std::string category;  // logger name ("core", "gpu", ...)
  std::string text;
  Level level;
};
// Appends the log lines with sequence >= `from` to `out`; returns the next sequence to ask for.
uint64_t CopyLogSince(uint64_t from, std::vector<LogLine>& out);

constexpr size_t kFrameHistory = 240;
struct Hitch {
  std::string time;
  float ms;
  std::string cause;
};
struct FrameStats {
  double fps = 0, avg_ms = 0, worst_ms = 0, low_1pct_fps = 0;
  double jitter_ms = 0;  // standard deviation of the recent frame times (pacing evenness)
  uint64_t frames = 0;
  std::array<float, kFrameHistory> history{};  // oldest first
  std::vector<Hitch> hitches;                  // newest first
};
FrameStats GetFrameStats();

struct FileActivity {
  std::string path;
  double megabytes;
  double seconds;  // open: since last read; recent: load duration
};
struct LoadStats {
  double read_rate_mbs = 0, total_read_mb = 0;
  std::vector<FileActivity> open, recent;  // recent: newest first
};
LoadStats GetLoadStats();

struct ThreadUsage {
  uint32_t id;
  std::string name;
  double cpu_percent;  // of one core
};
// CPU usage of the game threads, refreshed every second while this keeps being called.
std::vector<ThreadUsage> GetThreadUsage();

std::vector<std::pair<std::string, bool>> GetNotices();  // text, is_error

// --- In-game UI (bo1_debug_ui.cpp) ---------------------------------------------------------------
std::unique_ptr<rex::ui::ImGuiDialog> CreateUi(rex::ui::ImGuiDrawer* drawer, const char* title);
void ToggleConsole();
// Opens the developer console on a tab ("console", "performance", "loading", "engine", "dvars").
void OpenConsole(const std::string& tab);
void ToggleOverlay();
// True while the developer console has the keyboard: the game must not receive input.
bool ConsoleOpen();

}  // namespace bo1::debug
