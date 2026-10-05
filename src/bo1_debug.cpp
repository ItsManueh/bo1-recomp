// bo1 - debugging system: log capture, frame timing, load tracking, thread usage and commands

#include "bo1_debug.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

#include "bo1_engine.h"
#include "bo1_players.h"
#include "bo1_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace bo1::debug {

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
double Megabytes(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

std::string ClockTime(std::chrono::system_clock::time_point tp) {
  auto t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_s(&tm, &t);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
  return fmt::format("{:02}:{:02}:{:02}.{:03}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count());
}

Level ToLevel(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace: return Level::kTrace;
    case spdlog::level::debug: return Level::kDebug;
    case spdlog::level::info: return Level::kInfo;
    case spdlog::level::warn: return Level::kWarn;
    default: return Level::kError;
  }
}

// Readable path: without the device prefix and trimmed on the left.
std::string ShortPath(const std::string& path, size_t max_len = 60) {
  std::string p = path;
  for (const char* prefix : {"game:\\", "update:\\", "d:\\", "D:\\", "\\Device\\Cdrom0\\"}) {
    if (p.rfind(prefix, 0) == 0) {
      p = p.substr(std::char_traits<char>::length(prefix));
      break;
    }
  }
  if (p.size() > max_len) p = "..." + p.substr(p.size() - (max_len - 3));
  return p;
}

// --- Log capture ---------------------------------------------------------------------------------

constexpr size_t kMaxLogLines = 6000;
constexpr size_t kMaxNotices = 6;

std::mutex g_log_mutex;
std::deque<LogLine> g_log;
uint64_t g_log_next = 0;
std::deque<std::pair<std::string, bool>> g_notices;

void PushNotice(std::string text, bool error) {
  std::lock_guard lock(g_log_mutex);
  g_notices.emplace_back(std::move(text), error);
  while (g_notices.size() > kMaxNotices) g_notices.pop_front();
}

// Keeps the latest log lines for the in-game console, and copies warnings and errors (except the
// kernel stubs, which say nothing useful) to the overlay.
class CaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::string text(msg.payload.data(), msg.payload.size());
    Level level = ToLevel(msg.level);
    bool notice = level >= Level::kWarn && text.find("STUB") == std::string::npos &&
                  text.find("Stub ") == std::string::npos;
    if (notice) PushNotice(text.substr(0, 110), level == Level::kError);
    std::lock_guard lock(g_log_mutex);
    g_log.push_back({g_log_next++, ClockTime(msg.time),
                     std::string(msg.logger_name.data(), msg.logger_name.size()),
                     std::move(text), level});
    while (g_log.size() > kMaxLogLines) g_log.pop_front();
  }
  void flush_() override {}
};

// --- Windows console window --------------------------------------------------------------------------
//
// Writing to a Windows console is slow (milliseconds with colors) and blocks the logging thread,
// often the game or GPU thread: messages are queued and written by a thread of their own.
class WindowsConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  WindowsConsoleSink() { std::thread([this] { WriterLoop(); }).detach(); }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    Line line{std::string(formatted.data(), formatted.size()), msg.level, msg.color_range_start,
              msg.color_range_end};
    {
      std::lock_guard lock(queue_mutex_);
      if (queue_.size() >= kMaxQueued) {
        ++dropped_;
        return;
      }
      queue_.push_back(std::move(line));
    }
    queue_cv_.notify_one();
  }
  void flush_() override {}

 private:
  struct Line {
    std::string text;
    spdlog::level::level_enum level;
    size_t color_start, color_end;
  };
  static constexpr size_t kMaxQueued = 4096;

  static WORD LevelColor(spdlog::level::level_enum level) {
    constexpr WORD kWhite = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    switch (level) {
      case spdlog::level::trace: return kWhite;
      case spdlog::level::debug: return FOREGROUND_GREEN | FOREGROUND_BLUE;
      case spdlog::level::info: return FOREGROUND_GREEN;
      case spdlog::level::warn: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
      default: return FOREGROUND_RED | FOREGROUND_INTENSITY;
    }
  }

  void WriterLoop() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(out, &info);
    const WORD normal = info.wAttributes;
    auto write = [out](const char* text, size_t size) {
      DWORD written = 0;
      WriteConsoleA(out, text, DWORD(size), &written, nullptr);
    };
    std::deque<Line> batch;
    for (;;) {
      uint64_t dropped;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !queue_.empty(); });
        batch.swap(queue_);
        dropped = std::exchange(dropped_, 0);
      }
      for (const Line& line : batch) {
        size_t start = std::min(line.color_start, line.text.size());
        size_t end = std::clamp(line.color_end, start, line.text.size());
        write(line.text.data(), start);
        SetConsoleTextAttribute(out, LevelColor(line.level));
        write(line.text.data() + start, end - start);
        SetConsoleTextAttribute(out, normal);
        write(line.text.data() + end, line.text.size() - end);
      }
      batch.clear();
      if (dropped) {
        std::string text = fmt::format(
            "({} messages skipped in this window; they are in the log file)\n", dropped);
        write(text.data(), text.size());
      }
    }
  }

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Line> queue_;
  uint64_t dropped_ = 0;
};

std::shared_ptr<WindowsConsoleSink> g_windows_console;

void WindowsConsoleInputLoop() {
  HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (input == INVALID_HANDLE_VALUE) return;
  std::wstring buffer(512, L'\0');
  for (;;) {
    DWORD read = 0;
    if (!ReadConsoleW(input, buffer.data(), DWORD(buffer.size()), &read, nullptr) || !read) {
      Sleep(100);
      continue;
    }
    int bytes =
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), int(read), nullptr, 0, nullptr, nullptr);
    std::string line(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), int(read), line.data(), bytes, nullptr,
                        nullptr);
    RunCommand(line);
  }
}

// --- Frames ----------------------------------------------------------------------------------------

constexpr size_t kMaxHitches = 40;
constexpr double kHitchMs = 50.0;

std::mutex g_frames_mutex;
std::array<float, kFrameHistory> g_frame_ms{};
size_t g_frame_pos = 0;
uint64_t g_frame_count = 0;
Clock::time_point g_last_frame;
// 1 s window for the UI and 10 s window for the log.
struct Window {
  Clock::time_point start;
  uint64_t frames = 0;
  double worst_ms = 0;
};
Window g_second, g_ten;
double g_fps = 0, g_avg_ms = 0, g_worst_ms = 0;
std::deque<Hitch> g_hitches;

// --- Loads -----------------------------------------------------------------------------------------

struct OpenFile {
  std::string path;
  uint64_t bytes = 0;
  Clock::time_point opened, last_read;
};
struct DoneLoad {
  std::string path;
  uint64_t bytes;
  double seconds;
};
constexpr size_t kMaxRecentLoads = 24;
std::mutex g_files_mutex;
std::unordered_map<uint32_t, OpenFile> g_open_files;
std::deque<DoneLoad> g_recent_loads;
std::atomic<uint64_t> g_bytes_read{0};
uint64_t g_bytes_at_second = 0, g_bytes_at_ten = 0;
uint64_t g_underruns_at_ten = 0;
double g_read_rate = 0;  // MB/s over the last second

// --- Thread usage ----------------------------------------------------------------------------------

std::mutex g_threads_mutex;
std::vector<ThreadUsage> g_thread_usage;
std::atomic<int64_t> g_thread_usage_wanted_at{0};  // steady clock ms of the last request
std::once_flag g_thread_monitor_once;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

std::string ThreadName(HANDLE thread) {
  PWSTR description = nullptr;
  std::string name;
  if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
    int n = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
    name.resize(n > 0 ? size_t(n - 1) : 0);
    WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), n, nullptr, nullptr);
    LocalFree(description);
  }
  return name;
}

uint64_t ThreadCpu100ns(HANDLE thread) {
  FILETIME created, exited, kernel_time, user_time;
  if (!GetThreadTimes(thread, &created, &exited, &kernel_time, &user_time)) return 0;
  auto to_u64 = [](const FILETIME& ft) {
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  };
  return to_u64(kernel_time) + to_u64(user_time);
}

// Samples every thread of the process once per second while the UI keeps asking for it.
void ThreadMonitorLoop() {
  std::map<DWORD, uint64_t> previous;
  auto previous_time = Clock::now();
  const DWORD pid = GetCurrentProcessId();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (NowMs() - g_thread_usage_wanted_at.load() > 3000) {
      previous.clear();
      continue;
    }
    auto now = Clock::now();
    double elapsed_100ns = Seconds(now - previous_time) * 1e7;
    previous_time = now;
    std::map<DWORD, uint64_t> current;
    std::vector<ThreadUsage> usage;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) continue;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid) continue;
      HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
      if (!thread) continue;
      uint64_t cpu = ThreadCpu100ns(thread);
      current[entry.th32ThreadID] = cpu;
      auto it = previous.find(entry.th32ThreadID);
      if (it != previous.end() && elapsed_100ns > 0) {
        double percent = double(cpu - it->second) * 100.0 / elapsed_100ns;
        if (percent >= 0.5) {
          std::string name = ThreadName(thread);
          usage.push_back({entry.th32ThreadID,
                           name.empty() ? fmt::format("thread {}", entry.th32ThreadID) : name,
                           percent});
        }
      }
      CloseHandle(thread);
    }
    CloseHandle(snapshot);
    previous.swap(current);
    std::sort(usage.begin(), usage.end(),
              [](const ThreadUsage& a, const ThreadUsage& b) { return a.cpu_percent > b.cpu_percent; });
    std::lock_guard lock(g_threads_mutex);
    g_thread_usage = std::move(usage);
  }
}

// --- Commands --------------------------------------------------------------------------------------

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

void PrintHelp() {
  REXLOG_INFO(
      "bo1: developer console commands:\n"
      "  help                 this list\n"
      "  fps                  frame rate, average and worst frame time\n"
      "  loads                open files and recent loads\n"
      "  threads              CPU usage of the busiest threads\n"
      "  dvar <name>          engine variable: value, default, type and description\n"
      "  dvars <filter>       engine variables whose name contains <filter>\n"
      "  dev on|off           engine developer mode (bo1_dev_commands or 'developer 1;\n"
      "                       developer_script 1'; the retail game can fail its checks)\n"
      "  press <p> <button>   holds a controller button of player p (1-4) for 150 ms\n"
      "  level <level>        minimum log level shown (trace, debug, info, warn, error)\n"
      "  overlay              shows or hides the compact overlay (F2)\n"
      "  console              shows or hides the developer console (F1)\n"
      "  anything else is sent to the game engine as a console command\n"
      "  (e.g. 'map zombie_theater', 'cg_fov 75', 'quit')");
}

}  // namespace

// --- Setup ---------------------------------------------------------------------------------------

void Initialize() {
  rex::AddSink(std::make_shared<CaptureSink>());
}

void OpenWindowsConsole(const char* title) {
  if (!GetConsoleWindow() && !AllocConsole()) {
    REXLOG_WARN("bo1: could not open the Windows console window");
    return;
  }
  SetConsoleTitleA(title);
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  // Closing the console window would kill the game: its close button is removed.
  if (HWND console = GetConsoleWindow()) {
    if (HMENU menu = GetSystemMenu(console, FALSE)) DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
  }
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(out, &info)) {
    info.dwSize.Y = 9999;
    SetConsoleScreenBufferSize(out, info.dwSize);
  }
  FILE* ignored = nullptr;
  freopen_s(&ignored, "CONOUT$", "w", stdout);
  freopen_s(&ignored, "CONOUT$", "w", stderr);

  g_windows_console = std::make_shared<WindowsConsoleSink>();
  g_windows_console->set_pattern("%H:%M:%S.%e %^%-5l%$ [%n] %v");
  g_windows_console->set_level(spdlog::level::from_str(REXCVAR_GET(bo1_console_level)));
  rex::AddSink(g_windows_console);
  rex::cvar::RegisterChangeCallback("bo1_console_level", [](std::string_view, std::string_view v) {
    if (g_windows_console) g_windows_console->set_level(spdlog::level::from_str(std::string(v)));
  });
  std::thread(WindowsConsoleInputLoop).detach();
  REXLOG_INFO("bo1: Windows console ready; type 'help' for the commands");
}

// --- Hooks -----------------------------------------------------------------------------------------

void Notice(const std::string& text) { PushNotice(text, true); }

void OnGameFrame() {
  auto now = Clock::now();
  std::lock_guard lock(g_frames_mutex);
  if (g_frame_count++ == 0) {
    g_last_frame = g_second.start = g_ten.start = now;
    return;
  }
  double ms = Seconds(now - g_last_frame) * 1000.0;
  g_last_frame = now;
  // Hitches: frames over 50 ms (except map loads, which stop the loop for seconds on purpose),
  // with whatever was being read from disk at that moment.
  if (ms > kHitchMs && ms < 2000.0) {
    std::string reading;
    {
      std::lock_guard files_lock(g_files_mutex);
      for (const auto& [handle, file] : g_open_files) {
        if (Seconds(now - file.last_read) * 1000.0 < ms) {
          reading += (reading.empty() ? "" : ", ") + ShortPath(file.path, 30);
        }
      }
    }
    std::string cause = reading.empty() ? "no disk reads" : "reading " + reading;
    REXLOG_WARN("bo1: hitch {:.0f} ms ({})", ms, cause);
    g_hitches.push_front({ClockTime(std::chrono::system_clock::now()), float(ms), cause});
    while (g_hitches.size() > kMaxHitches) g_hitches.pop_back();
  }
  g_frame_ms[g_frame_pos] = float(ms);
  g_frame_pos = (g_frame_pos + 1) % kFrameHistory;
  for (Window* w : {&g_second, &g_ten}) {
    ++w->frames;
    w->worst_ms = std::max(w->worst_ms, ms);
  }
  uint64_t bytes = g_bytes_read.load(std::memory_order_relaxed);
  double second = Seconds(now - g_second.start);
  if (second >= 1.0) {
    g_fps = g_second.frames / second;
    g_avg_ms = second * 1000.0 / double(std::max<uint64_t>(g_second.frames, 1));
    g_worst_ms = g_second.worst_ms;
    g_read_rate = Megabytes(bytes - g_bytes_at_second) / second;
    g_bytes_at_second = bytes;
    g_second = {now, 0, 0};
  }
  double ten = Seconds(now - g_ten.start);
  if (ten >= 10.0) {
    // Audio: output latency (queued game audio + device buffer) and gaps where the device found
    // nothing queued in these 10 s.
    const uint64_t underruns = rex::audio::GetOutputUnderruns();
    REXLOG_INFO("bo1: average FPS over the last {:.0f} s = {:.1f} | worst frame {:.1f} ms | "
                "read {:.1f} MB | audio latency {:.0f} ms, {} gaps",
                ten, g_ten.frames / ten, g_ten.worst_ms, Megabytes(bytes - g_bytes_at_ten),
                rex::audio::GetOutputLatencyMs(), underruns - g_underruns_at_ten);
    g_underruns_at_ten = underruns;
    g_bytes_at_ten = bytes;
    g_ten = {now, 0, 0};
  }
}

void OnFileOpened(uint32_t handle, const std::string& path) {
  auto now = Clock::now();
  REXLOG_DEBUG("bo1: open {}", path);
  std::lock_guard lock(g_files_mutex);
  g_open_files[handle] = OpenFile{path, 0, now, now};
}

void OnFileRead(uint32_t handle, uint64_t bytes) {
  g_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
  std::lock_guard lock(g_files_mutex);
  auto it = g_open_files.find(handle);
  if (it != g_open_files.end()) {
    it->second.bytes += bytes;
    it->second.last_read = Clock::now();
  }
}

void OnFileClosed(uint32_t handle) {
  std::lock_guard lock(g_files_mutex);
  auto it = g_open_files.find(handle);
  if (it == g_open_files.end()) return;
  const OpenFile& file = it->second;
  if (file.bytes) {
    double seconds = Seconds(Clock::now() - file.opened);
    // Large loads (zones, videos) are logged at info; small ones only at debug.
    if (file.bytes >= 256 * 1024) {
      REXLOG_INFO("bo1: loaded {} ({:.1f} MB in {:.2f} s)", ShortPath(file.path, 80),
                  Megabytes(file.bytes), seconds);
    } else {
      REXLOG_DEBUG("bo1: loaded {} ({} bytes)", file.path, file.bytes);
    }
    g_recent_loads.push_front({file.path, file.bytes, seconds});
    while (g_recent_loads.size() > kMaxRecentLoads) g_recent_loads.pop_back();
  }
  g_open_files.erase(it);
}

// --- Commands --------------------------------------------------------------------------------------

const std::vector<std::string>& BuiltinCommands() {
  static const std::vector<std::string> kCommands = {"help",  "fps",   "loads", "threads",
                                                     "dvar",  "dvars", "dev",   "level",
                                                     "overlay", "console", "press"};
  return kCommands;
}

void RunCommand(const std::string& raw_line) {
  std::string line = raw_line;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.pop_back();
  }
  size_t start = line.find_first_not_of(' ');
  if (start == std::string::npos) return;
  line = line.substr(start);
  std::string word = Lower(line.substr(0, line.find(' ')));
  std::string arg = line.size() > word.size() ? line.substr(word.size() + 1) : "";
  arg.erase(0, arg.find_first_not_of(' '));
  REXLOG_INFO("bo1: > {}", line);

  if (word == "help" || word == "?") {
    PrintHelp();
  } else if (word == "fps") {
    FrameStats stats = GetFrameStats();
    REXLOG_INFO("bo1: {:.1f} FPS, {:.2f} ms average, jitter {:.2f} ms, worst frame {:.2f} ms, "
                "1% low {:.1f} FPS ({} frames)",
                stats.fps, stats.avg_ms, stats.jitter_ms, stats.worst_ms, stats.low_1pct_fps,
                stats.frames);
  } else if (word == "loads") {
    LoadStats loads = GetLoadStats();
    REXLOG_INFO("bo1: reading {:.1f} MB/s, {:.1f} MB in total, {} files open", loads.read_rate_mbs,
                loads.total_read_mb, loads.open.size());
    for (const auto& file : loads.open) {
      REXLOG_INFO("bo1:   open {} ({:.1f} MB read, last read {:.1f} s ago)", file.path,
                  file.megabytes, file.seconds);
    }
    for (const auto& file : loads.recent) {
      REXLOG_INFO("bo1:   loaded {} ({:.1f} MB in {:.2f} s)", file.path, file.megabytes,
                  file.seconds);
    }
  } else if (word == "threads") {
    auto threads = GetThreadUsage();
    if (threads.empty()) REXLOG_INFO("bo1: measuring thread usage, run 'threads' again in 2 s");
    for (size_t i = 0; i < std::min<size_t>(threads.size(), 12); ++i) {
      REXLOG_INFO("bo1:   {:5.1f}%  {}", threads[i].cpu_percent, threads[i].name);
    }
  } else if (word == "dvar") {
    if (auto dvar = engine::FindDvar(arg)) {
      REXLOG_INFO("bo1: {} = \"{}\" (default \"{}\", {}{}{}, flags {:#x}) {}", dvar->name,
                  dvar->value, dvar->reset, dvar->type, dvar->range.empty() ? "" : ", ",
                  dvar->range, dvar->flags, dvar->description);
    } else {
      REXLOG_WARN("bo1: no dvar named '{}'", arg);
    }
  } else if (word == "dvars") {
    auto start = std::chrono::steady_clock::now();
    auto dvars = engine::ListDvars(arg, 200);
    REXLOG_DEBUG("bo1: dvar list took {:.2f} ms",
                 Seconds(std::chrono::steady_clock::now() - start) * 1000.0);
    for (const auto& dvar : dvars) {
      REXLOG_INFO("bo1:   {} = \"{}\"", dvar.name, dvar.value);
    }
    REXLOG_INFO("bo1: {} dvars{}", dvars.size(), dvars.size() == 200 ? " (first 200)" : "");
  } else if (word == "dev") {
    if (Lower(arg) == "off") {
      engine::ExecuteCommand("developer 0");
      engine::ExecuteCommand("developer_script 0");
    } else {
      std::string commands = REXCVAR_GET(bo1_dev_commands);
      if (commands.empty()) commands = "developer 1;developer_script 1";
      REXLOG_WARN("bo1: engine developer mode on ({}): developer checks and script asserts can "
                  "stop the retail game; 'dev off' turns it off",
                  commands);
      for (size_t pos = 0; pos <= commands.size();) {
        size_t end = std::min(commands.find(';', pos), commands.size());
        std::string command = commands.substr(pos, end - pos);
        if (!command.empty()) engine::ExecuteCommand(command);
        pos = end + 1;
      }
    }
  } else if (word == "press") {
    // press <player 1-4> <button> [milliseconds]
    std::istringstream args(arg);
    int player = 0, hold_ms = 150;
    std::string button;
    args >> player >> button >> hold_ms;
    if (player < 1 || player > 4 || !players::Press(uint32_t(player - 1), Lower(button), hold_ms)) {
      REXLOG_WARN("bo1: usage: press <player 1-4> <a|b|x|y|start|back|up|down|left|right|lb|rb|"
                  "ls|rs|lt|rt> [milliseconds]");
    } else {
      REXLOG_INFO("bo1: player {} presses {} for {} ms", player, Lower(button), hold_ms);
    }
  } else if (word == "level") {
    if (rex::cvar::SetFlagByName("bo1_console_level", arg)) {
      REXLOG_INFO("bo1: console level = {}", arg);
    } else {
      REXLOG_WARN("bo1: invalid level '{}'", arg);
    }
  } else if (word == "overlay") {
    ToggleOverlay();
  } else if (word == "console") {
    if (arg.empty()) {
      ToggleConsole();
    } else {
      OpenConsole(arg);
    }
  } else {
    engine::ExecuteCommand(line);
  }
}

// --- Snapshots -------------------------------------------------------------------------------------

uint64_t CopyLogSince(uint64_t from, std::vector<LogLine>& out) {
  std::lock_guard lock(g_log_mutex);
  if (g_log.empty()) return g_log_next;
  uint64_t first = g_log.front().sequence;
  for (uint64_t s = std::max(from, first); s < g_log_next; ++s) {
    out.push_back(g_log[size_t(s - first)]);
  }
  return g_log_next;
}

FrameStats GetFrameStats() {
  FrameStats stats;
  {
    std::lock_guard lock(g_frames_mutex);
    for (size_t i = 0; i < kFrameHistory; ++i) {
      stats.history[i] = g_frame_ms[(g_frame_pos + i) % kFrameHistory];
    }
    stats.fps = g_fps;
    stats.avg_ms = g_avg_ms;
    stats.worst_ms = g_worst_ms;
    stats.frames = g_frame_count;
    stats.hitches.assign(g_hitches.begin(), g_hitches.end());
  }
  // 1% low: frame rate of the slowest 1% of the recent frames.
  std::vector<float> sorted;
  double sum_ms = 0, sum_sq = 0;
  for (float ms : stats.history) {
    if (ms > 0) {
      sorted.push_back(ms);
      sum_ms += ms;
      sum_sq += double(ms) * ms;
    }
  }
  if (sorted.size() > 1) {
    double mean = sum_ms / double(sorted.size());
    stats.jitter_ms = std::sqrt(std::max(0.0, sum_sq / double(sorted.size()) - mean * mean));
  }
  if (!sorted.empty()) {
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());
    size_t count = std::max<size_t>(1, sorted.size() / 100);
    double sum = 0;
    for (size_t i = 0; i < count; ++i) sum += sorted[i];
    stats.low_1pct_fps = 1000.0 / (sum / double(count));
  }
  return stats;
}

LoadStats GetLoadStats() {
  LoadStats stats;
  auto now = Clock::now();
  std::lock_guard lock(g_files_mutex);
  stats.read_rate_mbs = g_read_rate;
  stats.total_read_mb = Megabytes(g_bytes_read.load());
  for (const auto& [handle, file] : g_open_files) {
    stats.open.push_back({ShortPath(file.path), Megabytes(file.bytes), Seconds(now - file.last_read)});
  }
  std::sort(stats.open.begin(), stats.open.end(),
            [](const FileActivity& a, const FileActivity& b) { return a.seconds < b.seconds; });
  for (const auto& load : g_recent_loads) {
    stats.recent.push_back({ShortPath(load.path), Megabytes(load.bytes), load.seconds});
  }
  return stats;
}

std::vector<ThreadUsage> GetThreadUsage() {
  g_thread_usage_wanted_at = NowMs();
  std::call_once(g_thread_monitor_once, [] { std::thread(ThreadMonitorLoop).detach(); });
  std::lock_guard lock(g_threads_mutex);
  return g_thread_usage;
}

std::vector<std::pair<std::string, bool>> GetNotices() {
  std::lock_guard lock(g_log_mutex);
  return {g_notices.begin(), g_notices.end()};
}

}  // namespace bo1::debug
