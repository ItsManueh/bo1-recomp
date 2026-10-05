// bo1 - local players: split screen profiles and input device assignment
//
// On the console every player of a split screen game holds a controller with a signed in profile:
// the game's split screen sign-in menu waits for one (PLATFORM_FEEDER_SECONDARY_CONTROLLER_SIGNIN)
// before it lets player 2 join. The runtime signs players 2-4 in to a local offline profile while
// a controller is connected to their slot, each with its own XUID, profile settings and saves.

#include "bo1_players.h"

#include <array>
#include <atomic>
#include <chrono>
#include <utility>

#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

REXCVAR_DEFINE_BOOL(bo1_split_screen, true, "Black Ops",
                    "Split screen: players 2-4 sign in to a local profile as soon as their "
                    "controller is connected (press START in the split screen menu to join)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_player_names, "Player 2,Player 3,Player 4", "Black Ops",
                      "Names of players 2, 3 and 4 in split screen, separated by commas (at most "
                      "15 ASCII characters each)");

REXCVAR_DEFINE_STRING(bo1_keyboard_player, "shared", "Black Ops",
                      "Keyboard and mouse: shared (player 1 together with the first controller) "
                      "or own (keyboard and mouse are player 1 and every controller is the next "
                      "player, so one person can play with the keyboard and another with a "
                      "controller)")
    .allowed({"shared", "own"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_split_screen_view, "console", "Black Ops",
                      "Multiplayer split screen views: console (each view narrowed by black side "
                      "bars, 10% of the width, as on the console) or full (each view uses the "
                      "whole half of the screen: wider field of view)")
    .allowed({"console", "full"});

// The engine waits for the emulated GPU to process the previous frame's commands, and the game
// threads that wait spin. With two views (twice the commands) the GPU command thread lost its core
// to them: 31-38 FPS at normal priority, 43-51 FPS at the highest (i5-9600K, 6 cores, with a
// browser and music running). With one view it is not needed, and there the menus' video players
// and the audio threads are better off without a higher priority thread in the way.
REXCVAR_DEFINE_BOOL(bo1_split_screen_boost, true, "Black Ops",
                    "Split screen performance: while two or more views are drawn, the emulated GPU "
                    "thread runs at the highest Windows priority (about 30% more frames per "
                    "second)");

namespace bo1::players {

namespace {

// Thread handle of the runtime's "GPU Commands" thread (found by name once).
HANDLE GpuCommandThread() {
  static HANDLE handle = [] {
    HANDLE found = nullptr;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok && !found;
         ok = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid) continue;
      HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION,
                                 FALSE, entry.th32ThreadID);
      if (!thread) continue;
      PWSTR description = nullptr;
      if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        if (wcsncmp(description, L"GPU Commands", 12) == 0) found = thread;
        LocalFree(description);
      }
      if (found != thread) CloseHandle(thread);
    }
    CloseHandle(snapshot);
    if (!found) REXLOG_WARN("bo1: GPU command thread not found, split screen boost unavailable");
    return found;
  }();
  return handle;
}

}  // namespace

void OnViewCount(int views) {
  static int boosted = -1;  // unknown
  const int want = REXCVAR_GET(bo1_split_screen_boost) && views >= 2 ? 1 : 0;
  if (want == boosted) return;
  HANDLE thread = GpuCommandThread();
  if (!thread) return;
  if (SetThreadPriority(thread, want ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL)) {
    if (boosted != -1 || want) {
      REXLOG_INFO("bo1: split screen with {} views: GPU command thread priority {}", views,
                  want ? "highest" : "normal");
    }
    boosted = want;
  }
}

std::vector<std::string> EngineCommands() {
  // Multiplayer only (the campaign executable has no such dvar and skips it).
  return {REXCVAR_GET(bo1_split_screen_view) == "full" ? "cg_splitscreenLetterboxSize 0"
                                                        : "cg_splitscreenLetterboxSize 0.1"};
}

void ApplyRuntimeOptions() {
  rex::cvar::SetFlagByName("xam_split_screen_profiles",
                           REXCVAR_GET(bo1_split_screen) ? "true" : "false");
  const bool own = REXCVAR_GET(bo1_keyboard_player) == "own";
  rex::cvar::SetFlagByName("input_first_pad_user", own ? "1" : "0");
  REXLOG_INFO("bo1: players: split screen {}, keyboard and mouse {}",
              REXCVAR_GET(bo1_split_screen) ? "on" : "off",
              own ? "are player 1 on their own (controllers are players 2-4)"
                  : "shared with the first controller");
}

namespace {

struct Injection {
  std::atomic<uint16_t> buttons{0};
  std::atomic<bool> left_trigger{false};
  std::atomic<bool> right_trigger{false};
  std::atomic<int64_t> until{0};  // steady clock, milliseconds
  // Changes when a press starts or ends, so the game sees a new input packet.
  std::atomic<uint32_t> sequence{0};
};
std::array<Injection, 4> g_injections;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

bool Press(uint32_t user_index, const std::string& button, int hold_ms) {
  static constexpr std::pair<const char*, uint16_t> kButtons[] = {
      {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
      {"start", 0x0010}, {"back", 0x0020}, {"ls", 0x0040},   {"rs", 0x0080},
      {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},    {"b", 0x2000},
      {"x", 0x4000},     {"y", 0x8000},    {"lt", 0},        {"rt", 0},
  };
  if (user_index >= g_injections.size()) return false;
  for (const auto& [name, bit] : kButtons) {
    if (button != name) continue;
    Injection& injection = g_injections[user_index];
    injection.buttons = bit;
    injection.left_trigger = button == "lt";
    injection.right_trigger = button == "rt";
    injection.until = NowMs() + std::max(hold_ms, 1);
    injection.sequence.fetch_add(1);
    return true;
  }
  return false;
}

bool InjectedInput(uint32_t user_index, uint16_t& buttons, uint8_t& left_trigger,
                   uint8_t& right_trigger, uint32_t& sequence) {
  if (user_index >= g_injections.size()) return false;
  Injection& injection = g_injections[user_index];
  sequence = injection.sequence.load();
  const int64_t until = injection.until.load();
  if (until == 0) return false;
  if (NowMs() >= until) {
    // Released: one more packet so the game sees the button go up.
    if (injection.until.exchange(0) != 0) injection.sequence.fetch_add(1);
    sequence = injection.sequence.load();
    return false;
  }
  buttons = injection.buttons.load();
  left_trigger = injection.left_trigger.load() ? 255 : 0;
  right_trigger = injection.right_trigger.load() ? 255 : 0;
  return true;
}

}  // namespace bo1::players
