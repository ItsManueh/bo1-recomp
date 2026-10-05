// bo1 - HLE kernel (xboxkrnl / xam) improvements for Black Ops
//
// Every kernel import the game uses is a weak symbol (__imp__Name) exported by the runtime.
// Defining it here makes the linker use this version instead of the runtime's, without rebuilding
// the SDK. When the original behaviour is needed, the runtime DLL export is called and the result
// is adjusted afterwards.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xtypes.h>

#include "bo1_debug.h"
#include "bo1_players.h"
#include "bo1_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bo1 {

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

// The SDK's X_ERROR_SUCCESS is a macro that depends on rex::X_RESULT; the value is enough here.
constexpr uint32_t kXSuccess = 0;

// Original implementation of a kernel export, looked up in the runtime DLL
// (rexruntime.dll / rexruntimerd.dll / rexruntimed.dll depending on the build configuration).
GuestFn OriginalExport(const char* name) {
  static const HMODULE runtime = [] {
    for (const wchar_t* dll : {L"rexruntimerd.dll", L"rexruntime.dll", L"rexruntimed.dll"}) {
      if (HMODULE m = GetModuleHandleW(dll)) return m;
    }
    return HMODULE{nullptr};
  }();
  auto fn = runtime ? reinterpret_cast<GuestFn>(GetProcAddress(runtime, name)) : nullptr;
  if (!fn) {
    REXLOG_ERROR("bo1: original export {} not found", name);
  }
  return fn;
}

// Player name shown in game: printable ASCII, at most 15 characters (Xbox Live limit).
std::string SanitizeName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (c >= 0x20 && c < 0x7F) out += c;
    if (out.size() == 15) break;
  }
  // No leading or trailing spaces (they come from the comma separated list).
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string Gamertag() {
  std::string out = SanitizeName(REXCVAR_GET(bo1_gamertag));
  return out.empty() ? std::string("Player") : out;
}

// Name of a local player: player 1 is the gamertag, players 2-4 (split screen) come from
// bo1_player_names.
std::string PlayerName(uint32_t user_index) {
  if (user_index == 0) return Gamertag();
  const std::string list = REXCVAR_GET(bo1_player_names);
  size_t start = 0;
  for (uint32_t i = 1; start <= list.size(); ++i) {
    size_t end = std::min(list.find(',', start), list.size());
    if (i == user_index) {
      std::string name = SanitizeName(std::string_view(list).substr(start, end - start));
      if (!name.empty()) return name;
      break;
    }
    start = end + 1;
  }
  return "Player " + std::to_string(user_index + 1);
}

}  // namespace

}  // namespace bo1

// --- Profile: player name -----------------------------------------------------------------------

// u32 XamUserGetName(u32 user_index, char* buffer, u32 buffer_size)
REX_HOOK_RAW(__imp__XamUserGetName) {
  static const auto original = bo1::OriginalExport("__imp__XamUserGetName");
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t buffer_size = ctx.r5.u32;
  if (original) original(ctx, base);
  if (ctx.r3.u32 != bo1::kXSuccess || user_index > 3 || !buffer || !buffer_size) return;
  std::string name = bo1::PlayerName(user_index);
  static std::once_flag once[4];
  std::call_once(once[user_index],
                 [&] { REXLOG_INFO("bo1: XamUserGetName({}) -> '{}'", user_index + 1, name); });
  name.resize(std::min<size_t>(name.size(), buffer_size - 1));
  auto* dst = rex::memory::GuestPtr<char*>(base, buffer);
  std::memcpy(dst, name.c_str(), name.size() + 1);
}

// u32 XamUserGetSigninInfo(u32 user_index, u32 flags, X_USER_SIGNIN_INFO* info)
// X_USER_SIGNIN_INFO: xuid(8) flags(4) signin_state(4) guest_number(4) sponsor(4) name[16]
REX_HOOK_RAW(__imp__XamUserGetSigninInfo) {
  static const auto original = bo1::OriginalExport("__imp__XamUserGetSigninInfo");
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t info = ctx.r5.u32;
  if (original) original(ctx, base);
  if (ctx.r3.u32 != bo1::kXSuccess || user_index > 3 || !info) return;
  std::array<char, 16> name{};
  std::string tag = bo1::PlayerName(user_index);
  static std::once_flag once[4];
  std::call_once(once[user_index], [&] {
    REXLOG_INFO("bo1: XamUserGetSigninInfo({}) -> '{}'", user_index + 1, tag);
  });
  std::memcpy(name.data(), tag.c_str(), std::min<size_t>(tag.size(), 15));
  std::memcpy(rex::memory::GuestPtr<char*>(base, info + 0x18), name.data(), name.size());
}

// --- Controllers: test input ---------------------------------------------------------------------

// u32 XamInputGetState(u32 user_index, u32 flags, X_INPUT_STATE* state)
// X_INPUT_STATE: packet_number(4) buttons(2) left_trigger(1) right_trigger(1) thumbs(8), big-endian.
// Adds the buttons held with the "press" console command (bo1::players::Press) to the real
// controller state; automated tests use it to get past "Press START" and to join split screen.
REX_HOOK_RAW(__imp__XamInputGetState) {
  static const auto original = bo1::OriginalExport("__imp__XamInputGetState");
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  if (original) original(ctx, base);
  if (ctx.r3.u32 != bo1::kXSuccess || !state || user_index > 3) return;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  uint32_t sequence = 0;
  const bool held =
      bo1::players::InjectedInput(user_index, buttons, left_trigger, right_trigger, sequence);
  if (sequence == 0) return;  // nothing was ever injected for this player
  auto* p = rex::memory::GuestPtr<uint8_t*>(base, state);
  const uint32_t packet = (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
                           uint32_t(p[3])) +
                          sequence;
  p[0] = uint8_t(packet >> 24);
  p[1] = uint8_t(packet >> 16);
  p[2] = uint8_t(packet >> 8);
  p[3] = uint8_t(packet);
  if (!held) return;
  p[4] |= uint8_t(buttons >> 8);
  p[5] |= uint8_t(buttons);
  p[6] = std::max(p[6], left_trigger);
  p[7] = std::max(p[7], right_trigger);
}

// --- Switching executables: campaign <-> multiplayer -------------------------------------------
//
// On the console, "Multiplayer" in the campaign menu calls XamLoaderLaunchTitle("default_mp.xex")
// and the system loads the other executable, handing it data set with XamLoaderSetLaunchData. Here
// each executable is a separate recompiled program (bo1.exe / bo1mp.exe), so the other .exe is
// started with the same arguments and the data travels through a temporary file in the save folder.

namespace bo1 {

namespace {

std::filesystem::path g_launch_data_file;
// Latest SetLaunchData payload of this process. It is only written to the file when the other
// executable is launched: multiplayer sets it right at startup and it must not reach a campaign
// that is opened by hand later.
std::mutex g_launch_data_mutex;
std::string g_pending_launch_data;

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// Current command line arguments without the program name.
std::wstring CurrentArguments() {
  std::wstring cmd = GetCommandLineW();
  size_t i = 0;
  if (!cmd.empty() && cmd[0] == L'"') {
    i = cmd.find(L'"', 1);
    i = (i == std::wstring::npos) ? cmd.size() : i + 1;
  } else {
    i = cmd.find(L' ');
    if (i == std::wstring::npos) i = cmd.size();
  }
  while (i < cmd.size() && cmd[i] == L' ') ++i;
  // The game command line (--cl=...) is dropped: on the console a title started with
  // XamLoaderLaunchTitle does not receive it, and repeating it (e.g. "+map ...") can relaunch in a
  // loop.
  std::wstring result;
  while (i < cmd.size()) {
    size_t start = i;
    bool quoted = false;
    while (i < cmd.size() && (quoted || cmd[i] != L' ')) {
      if (cmd[i] == L'"') quoted = !quoted;
      ++i;
    }
    std::wstring token = cmd.substr(start, i - start);
    std::wstring bare = token;
    bare.erase(std::remove(bare.begin(), bare.end(), L'"'), bare.end());
    if (bare.rfind(L"--cl=", 0) != 0 && bare.rfind(L"-cl=", 0) != 0) {
      if (!result.empty()) result += L' ';
      result += token;
    }
    while (i < cmd.size() && cmd[i] == L' ') ++i;
  }
  return result;
}

[[noreturn]] void ExitForLaunch() {
  rex::FlushLogging();
  // Like the console: the current title ends as soon as the next one is launched.
  TerminateProcess(GetCurrentProcess(), 0);
  std::abort();
}

}  // namespace

void SetLaunchDataFile(const std::filesystem::path& file) {
  g_launch_data_file = file;
  // Stale data (e.g. after a crash) must not reach a normal start.
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    auto age = std::filesystem::file_time_type::clock::now() -
               std::filesystem::last_write_time(file, ec);
    if (age > std::chrono::minutes(2)) std::filesystem::remove(file, ec);
  }
}

}  // namespace bo1

// u32 XamLoaderSetLaunchData(void* data, u32 size)
REX_HOOK_RAW(__imp__XamLoaderSetLaunchData) {
  static const auto original = bo1::OriginalExport("__imp__XamLoaderSetLaunchData");
  const uint32_t data = ctx.r3.u32;
  const uint32_t size = ctx.r4.u32;
  if (original) original(ctx, base);
  std::lock_guard lock(bo1::g_launch_data_mutex);
  if (data && size) {
    bo1::g_pending_launch_data.assign(rex::memory::GuestPtr<const char*>(base, data), size);
  } else {
    bo1::g_pending_launch_data.clear();
  }
}

// u32 XamLoaderGetLaunchData(void* buffer, u32 buffer_size)
REX_HOOK_RAW(__imp__XamLoaderGetLaunchData) {
  static const auto original = bo1::OriginalExport("__imp__XamLoaderGetLaunchData");
  std::error_code ec;
  const auto& file = bo1::g_launch_data_file;
  if (!file.empty() && std::filesystem::exists(file, ec)) {
    std::ifstream in(file, std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(file, ec);
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t buffer_size = ctx.r4.u32;
    size_t n = std::min<size_t>(blob.size(), buffer_size);
    if (buffer && n) std::memcpy(rex::memory::GuestPtr<char*>(base, buffer), blob.data(), n);
    REXLOG_INFO("bo1: launch data received from the other executable ({} bytes)", n);
    ctx.r3.u64 = bo1::kXSuccess;
    return;
  }
  if (original) original(ctx, base);
}

// void XamLoaderLaunchTitle(const char* name, u32 flags)
REX_HOOK_RAW(__imp__XamLoaderLaunchTitle) {
  static const auto original = bo1::OriginalExport("__imp__XamLoaderLaunchTitle");
  std::string name =
      ctx.r3.u32 ? std::string(rex::memory::GuestPtr<const char*>(base, ctx.r3.u32)) : "";
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  const wchar_t* target = nullptr;
  if (lower.find("default_mp.xex") != std::string::npos) {
    target = L"bo1mp.exe";
  } else if (lower.find("default.xex") != std::string::npos) {
    target = L"bo1.exe";
  }

  if (!target) {
    // An empty name means "back to the dashboard": on PC, close the game.
    REXLOG_INFO("bo1: XamLoaderLaunchTitle('{}'): no matching executable, closing the game", name);
    bo1::ExitForLaunch();
  }
  auto exe = bo1::ExeDir() / target;
  if (!std::filesystem::exists(exe)) {
    REXLOG_ERROR("bo1: XamLoaderLaunchTitle('{}'): {} does not exist", name, exe.string());
    if (original) original(ctx, base);
    return;
  }
  {
    std::lock_guard lock(bo1::g_launch_data_mutex);
    std::error_code ec;
    if (!bo1::g_launch_data_file.empty()) {
      if (bo1::g_pending_launch_data.empty()) {
        std::filesystem::remove(bo1::g_launch_data_file, ec);
      } else {
        std::ofstream out(bo1::g_launch_data_file, std::ios::binary | std::ios::trunc);
        out.write(bo1::g_pending_launch_data.data(), bo1::g_pending_launch_data.size());
        REXLOG_INFO("bo1: launch data for {} ({} bytes)", std::filesystem::path(target).string(),
                    bo1::g_pending_launch_data.size());
      }
    }
  }
  // Loop protection: if the game keeps relaunching itself (e.g. its error recovery launches the
  // title again and it fails again), do not spawn processes forever. Consecutive relaunches from
  // processes that lived less than 2 minutes are counted (a normal campaign <-> multiplayer switch
  // happens after playing for a while); the count travels to the child process in an environment
  // variable and the chain stops at 4.
  {
    wchar_t chain[16] = {};
    unsigned count = 0;
    if (GetEnvironmentVariableW(L"BO1_LAUNCH_CHAIN", chain, 16)) {
      count = unsigned(wcstoul(chain, nullptr, 10));
    }
    FILETIME created, exited, kernel_time, user_time, now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    bool short_lived = false;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_time, &user_time)) {
      auto to_u64 = [](const FILETIME& ft) {
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
      };
      // FILETIME: 100 ns units.
      short_lived = to_u64(now_ft) - to_u64(created) < uint64_t(120) * 10000000;
    }
    count = short_lived ? count + 1 : 0;
    if (count >= 4) {
      REXLOG_ERROR("bo1: the game relaunched itself {} times in a row a few seconds after "
                   "starting ('{}'); stopping to avoid a loop",
                   count, name);
      rex::FlushLogging();
      MessageBoxW(nullptr,
                  L"The game tried to restart itself several times in a row right after "
                  L"starting, probably because of a loading error. It was stopped to avoid a "
                  L"loop. Check the log in the logs folder.",
                  L"Call of Duty: Black Ops (recompiled)", MB_OK | MB_ICONERROR);
      TerminateProcess(GetCurrentProcess(), 3);
    }
    swprintf_s(chain, L"%u", count);
    SetEnvironmentVariableW(L"BO1_LAUNCH_CHAIN", chain);
  }
  std::wstring cmdline = L"\"" + exe.wstring() + L"\" " + bo1::CurrentArguments();
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      exe.parent_path().c_str(), &si, &pi)) {
    REXLOG_ERROR("bo1: could not launch {} (error {})", exe.string(), GetLastError());
    if (original) original(ctx, base);
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  REXLOG_INFO("bo1: '{}' -> launched {}; this process exits", name, exe.filename().string());
  bo1::ExitForLaunch();
}

// --- Stubs replaced by silent implementations ---------------------------------------------------

// Xbox Guide custom actions (Xbox button menu). They do not exist on PC: success.
REX_HOOK_RAW(__imp__XCustomSetAction) {
  (void)base;
  ctx.r3.u64 = 0;
}

// The game dismounts the cache partition at startup. The runtime VFS has nothing to dismount (the
// cache is a PC folder): STATUS_SUCCESS without a warning.
REX_HOOK_RAW(__imp__IoDismountVolumeByFileHandle) {
  (void)base;
  ctx.r3.u64 = 0;
}

// --- Load tracking: which files the game opens and reads (debug console and overlay) -------------
//
// Observation only: the original kernel function is always called first, then the result noted.
//   NtCreateFile(handle*, access, OBJECT_ATTRIBUTES*, io_status*, ...)
//   NtOpenFile(handle*, access, OBJECT_ATTRIBUTES*, io_status*, share, options)
//   NtReadFile(handle, event, apc, apc_ctx, io_status*, buffer, length, offset*)
//   NtReadFileScatter(handle, event, apc, apc_ctx, io_status*, segments, length, offset*)
// OBJECT_ATTRIBUTES: root(4) ANSI_STRING*(4) attributes(4); ANSI_STRING: len(2) max(2) char*(4).

namespace bo1 {
namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusPending = 0x103;

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::string ObjectAttributesPath(uint8_t* base, uint32_t object_attributes) {
  if (!object_attributes) return {};
  uint32_t name = LoadBE32(base + object_attributes + 4);
  if (!name) return {};
  uint32_t length = (uint32_t(base[name]) << 8) | base[name + 1];
  uint32_t buffer = LoadBE32(base + name + 4);
  if (!buffer || !length) return {};
  return std::string(reinterpret_cast<const char*>(base + buffer),
                     std::min<uint32_t>(length, 260));
}

void TrackOpen(PPCContext& ctx, uint8_t* base, GuestFn original) {
  const uint32_t handle_out = ctx.r3.u32;
  std::string path = ObjectAttributesPath(base, ctx.r5.u32);
  if (original) original(ctx, base);
  if (ctx.r3.u32 == kStatusSuccess && handle_out && !path.empty()) {
    debug::OnFileOpened(LoadBE32(base + handle_out), path);
  }
}

void TrackRead(PPCContext& ctx, uint8_t* base, GuestFn original) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t length = ctx.r9.u32;
  if (original) original(ctx, base);
  if (ctx.r3.u32 == kStatusSuccess || ctx.r3.u32 == kStatusPending) {
    debug::OnFileRead(handle, length);
  }
}

}  // namespace
}  // namespace bo1

REX_HOOK_RAW(__imp__NtCreateFile) {
  static const auto original = bo1::OriginalExport("__imp__NtCreateFile");
  bo1::TrackOpen(ctx, base, original);
}

REX_HOOK_RAW(__imp__NtOpenFile) {
  static const auto original = bo1::OriginalExport("__imp__NtOpenFile");
  bo1::TrackOpen(ctx, base, original);
}

REX_HOOK_RAW(__imp__NtReadFile) {
  static const auto original = bo1::OriginalExport("__imp__NtReadFile");
  bo1::TrackRead(ctx, base, original);
}

REX_HOOK_RAW(__imp__NtReadFileScatter) {
  static const auto original = bo1::OriginalExport("__imp__NtReadFileScatter");
  bo1::TrackRead(ctx, base, original);
}

REX_HOOK_RAW(__imp__NtClose) {
  static const auto original = bo1::OriginalExport("__imp__NtClose");
  const uint32_t handle = ctx.r3.u32;
  if (original) original(ctx, base);
  bo1::debug::OnFileClosed(handle);
}

// --- KeGetCurrentProcessType ---------------------------------------------------------------------
//
// The game calls it ~800,000 times per second (inside its locks and waits). Same logic as the
// runtime, but reading the processor PCR straight from r13 instead of looking up the current
// thread in the host tables on every call.
//   X_KPCR: +0x0C processtype_value_in_dpc, +0x100 prcb.current_thread, +0x150 prcb.dpc_active
//   X_KTHREAD: +0x73 process_type
REX_HOOK_RAW(__imp__KeGetCurrentProcessType) {
  const uint8_t* pcr = base + ctx.r13.u32;
  uint32_t type;
  if (bo1::LoadBE32(pcr + 0x150)) {
    type = pcr[0x0C];
  } else {
    type = base[bo1::LoadBE32(pcr + 0x100) + 0x73];
  }
  ctx.r3.u64 = type;
}
