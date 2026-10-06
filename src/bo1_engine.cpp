// bo1 - game engine functions intercepted by the port and read-only access to the engine dvars
//
// Each translated function sub_X is a weak alias of __imp__sub_X, so defining sub_X here replaces
// it in every call while __imp__sub_X stays the original. The generated code is not touched.
// Title Update #11 addresses (found with tools/find_string_callers.py and tools/ppcdis.py):
//
//                       campaign (default.xex)   multiplayer (default_mp.xex)
//   Com_Frame           0x82315590               0x82343D60
//   Cbuf_AddText        0x8230FD58               0x8233E8D8   (client, text)
//   Com_Error           0x82313280               0x82341CA8   (code, format, ...)
//   Dvar_FindVar        0x82379648               0x823E2768   (hash -> dvar_t*)
//   dvar hash table     0x8334EE60               0x8399C600   (1024 buckets)
//
// Dvar lookup, as the engine does it: hash = 5381; for each char, hash = hash * 33 + tolower(c);
// bucket = hash & 1023; then follow dvar_t::hash_next comparing dvar_t::hash.

#include "bo1_engine.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

#include "bo1_audio.h"
#include "bo1_debug.h"
#include "bo1_framerate.h"
#include "bo1_graphics.h"
#include "bo1_players.h"
#include "bo1_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bo1::engine {

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

#if defined(BO1_MP)
constexpr uint32_t kDvarHashTable = 0x8399C600;
#else
constexpr uint32_t kDvarHashTable = 0x8334EE60;
#endif
constexpr uint32_t kDvarBuckets = 1024;

// dvar_t layout (Title Update #11, both executables).
constexpr uint32_t kDvarName = 0x00;         // const char*
constexpr uint32_t kDvarDescription = 0x04;  // const char*
constexpr uint32_t kDvarHash = 0x08;         // uint32
constexpr uint32_t kDvarFlags = 0x0C;        // uint32
constexpr uint32_t kDvarType = 0x10;         // uint32 dvarType
constexpr uint32_t kDvarCurrent = 0x18;      // DvarValue (16 bytes)
constexpr uint32_t kDvarReset = 0x38;        // DvarValue
constexpr uint32_t kDvarDomain = 0x58;       // DvarLimits (int: min, max; enum: count, strings*)
constexpr uint32_t kDvarHashNext = 0x68;     // dvar_t*

// dvar_t::flags bit of cheat protected dvars (r_fullbright, cg_thirdPerson...).
constexpr uint32_t kDvarFlagCheat = 0x800000;

constexpr uint32_t kCommandBufferSize = 1024;

rex::memory::Memory* g_memory = nullptr;
uint32_t g_command_buffer = 0;  // address in game memory

std::mutex g_commands_mutex;
std::deque<std::string> g_commands;

std::atomic<uint64_t> g_frame_count{0};

// Graphics and frame pacing dvars (bo1_graphics.cpp, bo1_framerate.cpp) are applied again after
// every map change and whenever one of those options changes.
constexpr uint64_t kMapCheckFrames = 30;
std::string g_current_map;
uint64_t g_apply_graphics_at = kMapCheckFrames;  // also once at startup (menus have no map)
std::atomic<bool> g_graphics_changed{false};
std::chrono::steady_clock::time_point g_test_exec_at;
std::deque<std::string> g_test_commands;  // bo1_test_exec, run on the main thread only

// --- Guest memory reads ----------------------------------------------------------------------------

// Whether [address, address + size) is committed, readable host memory. The last readable region
// is cached: dvars and their strings sit in a handful of regions.
bool Readable(uint32_t address, uint32_t size) {
  if (!g_memory || address < 0x10000 || uint64_t(address) + size > 0xFFFF0000ull) return false;
  thread_local uintptr_t cached_start = 0, cached_end = 0;
  auto host = reinterpret_cast<uintptr_t>(g_memory->virtual_membase()) + address;
  if (host >= cached_start && host + size <= cached_end) return true;
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<const void*>(host), &info, sizeof(info))) return false;
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
  if (info.State != MEM_COMMIT || !(info.Protect & kReadable) || (info.Protect & PAGE_GUARD)) {
    return false;
  }
  cached_start = reinterpret_cast<uintptr_t>(info.BaseAddress);
  cached_end = cached_start + info.RegionSize;
  return host + size <= cached_end;
}

const uint8_t* Guest(uint32_t address) { return g_memory->virtual_membase() + address; }

uint32_t Read32(uint32_t address) {
  if (!Readable(address, 4)) return 0;
  uint32_t value;
  std::memcpy(&value, Guest(address), 4);
  return std::byteswap(value);
}

float ReadFloat(uint32_t address) { return std::bit_cast<float>(Read32(address)); }

std::string ReadString(uint32_t address, size_t max_length = 256) {
  std::string out;
  for (size_t i = 0; i < max_length && Readable(address + uint32_t(i), 1); ++i) {
    char c = char(*Guest(address + uint32_t(i)));
    if (!c) break;
    out += (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) ? c
                                                                                           : '?';
  }
  return out;
}

uint32_t DvarHash(std::string_view name) {
  uint32_t hash = 5381;
  for (char c : name) {
    hash = hash * 33 + uint32_t(std::tolower(static_cast<unsigned char>(c)));
  }
  return hash;
}

const char* DvarTypeName(uint8_t type) {
  static const char* kNames[] = {"bool",  "float", "vec2",  "vec3", "vec4", "int",
                                 "enum",  "string", "color", "int64", "linear color",
                                 "color xyz"};
  return type < std::size(kNames) ? kNames[type] : "?";
}

std::string FormatDvarValue(uint32_t dvar, uint8_t type, uint32_t value_offset) {
  const uint32_t v = dvar + value_offset;
  switch (type) {
    case 0:
      return Readable(v, 1) && *Guest(v) ? "1" : "0";
    case 1:
      return fmt::format("{:g}", ReadFloat(v));
    case 2:
      return fmt::format("{:g} {:g}", ReadFloat(v), ReadFloat(v + 4));
    case 3:
      return fmt::format("{:g} {:g} {:g}", ReadFloat(v), ReadFloat(v + 4), ReadFloat(v + 8));
    case 4:
    case 10:
    case 11:
      return fmt::format("{:g} {:g} {:g} {:g}", ReadFloat(v), ReadFloat(v + 4),
                         ReadFloat(v + 8), ReadFloat(v + 12));
    case 5:
      return fmt::format("{}", int32_t(Read32(v)));
    case 6: {
      int32_t index = int32_t(Read32(v));
      uint32_t count = Read32(dvar + kDvarDomain);
      uint32_t strings = Read32(dvar + kDvarDomain + 4);
      if (index >= 0 && uint32_t(index) < count && count < 256 && strings) {
        return ReadString(Read32(strings + uint32_t(index) * 4));
      }
      return fmt::format("{}", index);
    }
    case 7:
      return ReadString(Read32(v));
    case 8:
      return Readable(v, 4) ? fmt::format("{} {} {} {}", Guest(v)[0], Guest(v)[1], Guest(v)[2],
                                          Guest(v)[3])
                            : "?";
    case 9:
      return fmt::format("{}", int64_t(uint64_t(Read32(v)) << 32 | Read32(v + 4)));
    default:
      return "?";
  }
}

// Allowed values of int, float and enum dvars (DvarLimits at kDvarDomain).
std::string FormatDvarDomain(uint32_t dvar, uint8_t type) {
  const uint32_t d = dvar + kDvarDomain;
  switch (type) {
    case 1:
      return fmt::format("{:g} .. {:g}", ReadFloat(d), ReadFloat(d + 4));
    case 5:
      return fmt::format("{} .. {}", int32_t(Read32(d)), int32_t(Read32(d + 4)));
    case 6: {
      uint32_t count = Read32(d);
      uint32_t strings = Read32(d + 4);
      std::string out;
      for (uint32_t i = 0; i < count && i < 32 && strings; ++i) {
        out += (i ? " " : "") + ReadString(Read32(strings + i * 4), 32);
      }
      return out;
    }
    default:
      return {};
  }
}

std::optional<DvarInfo> DescribeDvar(uint32_t dvar) {
  if (!Readable(dvar, kDvarHashNext + 4)) return std::nullopt;
  DvarInfo info;
  info.name = ReadString(Read32(dvar + kDvarName), 64);
  if (info.name.empty()) return std::nullopt;
  info.description = ReadString(Read32(dvar + kDvarDescription));
  uint8_t type = uint8_t(Read32(dvar + kDvarType));
  info.type = DvarTypeName(type);
  info.flags = Read32(dvar + kDvarFlags);
  info.value = FormatDvarValue(dvar, type, kDvarCurrent);
  info.reset = FormatDvarValue(dvar, type, kDvarReset);
  info.range = FormatDvarDomain(dvar, type);
  return info;
}

// Visits every dvar in the hash table; stops when `visit` returns false.
template <typename Visit>
void ForEachDvar(Visit&& visit) {
  if (!g_memory) return;
  for (uint32_t bucket = 0; bucket < kDvarBuckets; ++bucket) {
    uint32_t dvar = Read32(kDvarHashTable + bucket * 4);
    // Chains are short; the limit only guards against reading a table being rebuilt.
    for (int guard = 0; dvar && guard < 256; ++guard) {
      if (!visit(dvar)) return;
      dvar = Read32(dvar + kDvarHashNext);
    }
  }
}

// --- Engine commands -----------------------------------------------------------------------------

// Runs the queued commands on the game main thread at the start of Com_Frame, where the engine
// processes its own command buffer.
void RunPendingCommands(PPCContext& ctx, uint8_t* base, GuestFn cbuf_add_text) {
  std::deque<std::string> commands;
  {
    std::lock_guard lock(g_commands_mutex);
    commands.swap(g_commands);
  }
  if (commands.empty() || !g_command_buffer) return;
  PPCContext saved = ctx;
  for (std::string& command : commands) {
    command.resize(std::min<size_t>(command.size(), kCommandBufferSize - 2));
    command += '\n';
    std::memcpy(base + g_command_buffer, command.c_str(), command.size() + 1);
    ctx.r3.u64 = 0;  // local client 0
    ctx.r4.u64 = g_command_buffer;
    cbuf_add_text(ctx, base);
    ctx = saved;
  }
}

// Splits "a;b;c" and passes each non-empty command to `run`.
template <typename Run>
void ForEachCommand(const std::string& list, Run&& run) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = std::min(list.find(';', start), list.size());
    std::string command = list.substr(start, end - start);
    command.erase(0, command.find_first_not_of(' '));
    if (!command.empty()) run(command);
    start = end + 1;
  }
}

void FrameStart(PPCContext& ctx, uint8_t* base, GuestFn cbuf_add_text) {
#ifdef REXGLUE_ENABLE_PROFILING
  if (tracy::IsProfilerStarted()) FrameMark;
#endif
  framerate::WaitForNextFrame();
  debug::OnGameFrame();
  auto now = std::chrono::steady_clock::now();
  if (g_frame_count.fetch_add(1, std::memory_order_relaxed) == 0) {
    if (REXCVAR_GET(bo1_dev_mode)) {
      ForEachCommand(REXCVAR_GET(bo1_dev_commands), [](const std::string& command) {
        REXLOG_INFO("bo1: developer mode > {}", command);
        ExecuteCommand(command);
      });
    }
    ForEachCommand(REXCVAR_GET(bo1_test_exec),
                   [](const std::string& command) { g_test_commands.push_back(command); });
    g_test_exec_at = now + std::chrono::seconds(REXCVAR_GET(bo1_test_exec_delay));
  }
  // Test commands run through the developer console, so the port commands (dvar, fps...) work
  // too; "wait <seconds>" pauses the list.
  while (!g_test_commands.empty() && now >= g_test_exec_at) {
    std::string command = std::move(g_test_commands.front());
    g_test_commands.pop_front();
    if (command.rfind("wait ", 0) == 0) {
      g_test_exec_at = now + std::chrono::milliseconds(
                                 int64_t(std::atof(command.c_str() + 5) * 1000.0));
    } else {
      debug::RunCommand(command);
    }
  }
  const uint64_t frame = g_frame_count.load(std::memory_order_relaxed);
  if (frame % kMapCheckFrames == 0) {
    if (auto views = FindDvar("r_num_viewports")) {
      players::OnViewCount(std::atoi(views->value.c_str()));
    }
    auto map = FindDvar("mapname");
    std::string name = map ? map->value : std::string();
    if (name != g_current_map) {
      g_current_map = name;
      // A few frames later, once the map has set its own dvars.
      g_apply_graphics_at = frame + kMapCheckFrames;
      if (!name.empty()) REXLOG_INFO("bo1: map {}", name);
    }
  }
  if (frame == g_apply_graphics_at || g_graphics_changed.exchange(false)) {
    std::vector<std::string> commands = graphics::EngineCommands();
    for (std::string& command : framerate::EngineCommands()) commands.push_back(std::move(command));
    for (std::string& command : audio::EngineCommands()) commands.push_back(std::move(command));
    for (std::string& command : players::EngineCommands()) commands.push_back(std::move(command));
    for (const std::string& command : commands) {
      // Some settings only exist in one of the executables (e.g. the split screen letterbox is
      // multiplayer only).
      auto dvar = FindDvar(command.substr(0, command.find(' ')));
      if (!dvar) {
        REXLOG_DEBUG("bo1: settings: {} does not exist here, skipped", command);
        continue;
      }
#if defined(BO1_MP)
      // Multiplayer enforces cheat protection: changing such a dvar is an engine error that
      // drops to the menu (and relaunches the game). Those settings only apply to the campaign.
      if (dvar->flags & kDvarFlagCheat) {
        REXLOG_DEBUG("bo1: settings: {} is cheat protected in multiplayer, skipped", dvar->name);
        continue;
      }
#endif
      REXLOG_DEBUG("bo1: settings > {}", command);
      ExecuteCommand(command);
    }
  }
  RunPendingCommands(ctx, base, cbuf_add_text);
}

// Minimal printf over the game arguments (r5..r10; on PowerPC variadic arguments are passed in
// registers). Enough for engine error messages.
std::string FormatGuest(PPCContext& ctx, uint8_t* base, uint32_t format) {
  const PPCRegister* args[] = {&ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t next = 0;
  auto guest_string = [&](uint32_t address) -> std::string {
    if (!address) return "(null)";
    const char* s = reinterpret_cast<const char*>(base + address);
    return std::string(s, strnlen(s, 512));
  };
  std::string fmt = guest_string(format), out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c != '%') {
      if (static_cast<unsigned char>(c) >= 0x20 || c == '\n') out += c;
      continue;
    }
    // Skip flags, width, precision and length.
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0123456789.lhzI", fmt[j])) ++j;
    if (j >= fmt.size()) break;
    char conversion = fmt[j];
    i = j;
    if (conversion == '%') {
      out += '%';
      continue;
    }
    if (next >= std::size(args)) {
      out += '?';
      continue;
    }
    const PPCRegister& arg = *args[next++];
    char buf[64];
    switch (conversion) {
      case 's':
        out += guest_string(arg.u32);
        break;
      case 'd':
      case 'i':
        std::snprintf(buf, sizeof(buf), "%d", int32_t(arg.u32));
        out += buf;
        break;
      case 'u':
        std::snprintf(buf, sizeof(buf), "%u", arg.u32);
        out += buf;
        break;
      case 'x':
      case 'X':
      case 'p':
        std::snprintf(buf, sizeof(buf), conversion == 'X' ? "%X" : "%x", arg.u32);
        out += buf;
        break;
      case 'c':
        out += char(arg.u32);
        break;
      case 'f':
      case 'g': {
        double value;
        std::memcpy(&value, &arg.u64, sizeof(value));
        std::snprintf(buf, sizeof(buf), "%g", value);
        out += buf;
        break;
      }
      default:
        out += '?';
        break;
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

void LogEngineError(PPCContext& ctx, uint8_t* base) {
  static const char* kNames[] = {"fatal",      "drop",   "server disconnect",
                                 "disconnect", "script", "script (drop)",
                                 "localization"};
  uint32_t code = ctx.r3.u32;
  std::string message = FormatGuest(ctx, base, ctx.r4.u32);
  const char* name = code < std::size(kNames) ? kNames[code] : "?";
  REXLOG_ERROR("bo1: engine error ({} {}): {}", code, name, message);
  debug::Notice("Engine error (" + std::string(name) + "): " + message.substr(0, 90));
}

}  // namespace

void Initialize(rex::memory::Memory* memory) {
  g_memory = memory;
  for (const char* option : {"bo1_shadows", "bo1_lod", "bo1_frame_pacing", "bo1_subtitles",
                             "bo1_split_screen_view"}) {
    rex::cvar::RegisterChangeCallback(
        option, [](std::string_view, std::string_view) { g_graphics_changed = true; });
  }
  if (memory && !g_command_buffer) {
    g_command_buffer = memory->SystemHeapAlloc(kCommandBufferSize);
  }
  if (!g_command_buffer) {
    REXLOG_WARN("bo1: no buffer for engine commands; the console will not be able to send them");
  }
}

void ExecuteCommand(const std::string& command) {
  std::lock_guard lock(g_commands_mutex);
  g_commands.push_back(command);
}

std::optional<DvarInfo> FindDvar(std::string_view name) {
  if (!g_memory || name.empty()) return std::nullopt;
  const uint32_t hash = DvarHash(name);
  uint32_t dvar = Read32(kDvarHashTable + (hash & (kDvarBuckets - 1)) * 4);
  for (int guard = 0; dvar && guard < 256; ++guard) {
    if (Read32(dvar + kDvarHash) == hash) return DescribeDvar(dvar);
    dvar = Read32(dvar + kDvarHashNext);
  }
  return std::nullopt;
}

std::vector<DvarInfo> ListDvars(std::string_view filter, size_t max_count) {
  std::string needle(filter);
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  std::vector<DvarInfo> out;
  ForEachDvar([&](uint32_t dvar) {
    std::string name = ReadString(Read32(dvar + kDvarName), 64);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (!name.empty() && lower.find(needle) != std::string::npos) {
      if (auto info = DescribeDvar(dvar)) out.push_back(std::move(*info));
    }
    return out.size() < max_count;
  });
  std::sort(out.begin(), out.end(), [](const DvarInfo& a, const DvarInfo& b) {
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return out;
}

uint64_t FrameCount() { return g_frame_count.load(std::memory_order_relaxed); }

}  // namespace bo1::engine

// --- Replacements (one set per executable) ---------------------------------------------------------
//
// These addresses belong to Title Update #11. The bootstrap build (BO1_BOOTSTRAP, translated from
// the original executable only to dump the Title Update one) has other functions there: it leaves
// the engine alone.

#if !defined(BO1_BOOTSTRAP)

namespace {

// Native FindNameInTable (sub_8242EF88 campaign / sub_82465CE8 multiplayer): looks a name up in one
// of the engine's field tables, which the entity spawner uses for every key of every spawned
// entity ("classname", "spawnflags"...). The translated loop calls the translated case-insensitive
// compare for each entry, and spawning many entities at once (zombie rounds, map changes) spent
// tens of milliseconds there.
//   r3 = name, r4 = int* out, r5 = table index; tables at kTables + index * 88.
//   Entry: name, NUL, u16 value, s8 extra. Found: returns the value and writes the extra (sign
//   extended) to *out. Not found: returns 0 and leaves *out alone.
// Same comparison as the game's (sub_82385600): ASCII 'A'-'Z' fold to lower case, bytes compared
// unsigned; a null name never matches.
void FindNameInTable(PPCContext& ctx, uint8_t* base, uint32_t tables) {
  const uint32_t name = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t index = ctx.r5.u32;
  // Guest address to host, as the translated code does it (the 0xE0000000+ physical view is
  // mapped 4 KB further on Windows).
  auto host = [base](uint32_t address) {
    return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
  };
  auto byte = [&host](uint32_t address) { return *host(address); };
  auto fold = [](uint8_t c) { return uint8_t(c >= 'A' && c <= 'Z' ? c + 0x20 : c); };
  const uint8_t* table_pointer = host(tables + index * 88);
  uint32_t entry = uint32_t(table_pointer[0]) << 24 | uint32_t(table_pointer[1]) << 16 |
                   uint32_t(table_pointer[2]) << 8 | table_pointer[3];
  ctx.r3.u64 = 0;
  if (!byte(entry)) return;
  while (true) {
    uint32_t length = 0;
    while (byte(entry + length)) ++length;
    bool equal = name != 0;
    for (uint32_t i = 0; equal; ++i) {
      const uint8_t a = fold(byte(name + i));
      const uint8_t b = fold(byte(entry + i));
      if (a != b) equal = false;
      if (!a) break;
    }
    if (equal) {
      const uint32_t value = entry + length + 1;
      ctx.r3.u64 = (uint32_t(byte(value)) << 8) | byte(value + 1);
      const uint32_t extra = uint32_t(int32_t(int8_t(byte(value + 2))));
      uint8_t* out_pointer = host(out);
      out_pointer[0] = uint8_t(extra >> 24);
      out_pointer[1] = uint8_t(extra >> 16);
      out_pointer[2] = uint8_t(extra >> 8);
      out_pointer[3] = uint8_t(extra);
      return;
    }
    entry += length + 4;
    if (!byte(entry)) return;
  }
}

// The first calls run both the native lookup and the translated original and compare them (return
// value and the int it writes). Any difference switches back to the original for good.
constexpr int kFindNameChecks = 5000;
std::atomic<int> g_find_name_checks{kFindNameChecks};
std::atomic<bool> g_find_name_native{true};

void CheckedFindNameInTable(PPCContext& ctx, uint8_t* base, uint32_t tables,
                            bo1::engine::GuestFn original) {
  if (!g_find_name_native.load(std::memory_order_relaxed)) {
    original(ctx, base);
    return;
  }
  if (g_find_name_checks.load(std::memory_order_relaxed) <= 0) {
    FindNameInTable(ctx, base, tables);
    return;
  }
  const uint32_t out = ctx.r4.u32;
  auto* out_host = base + out + (out >= 0xE0000000u ? 0x1000u : 0u);
  uint8_t before[4];
  std::memcpy(before, out_host, 4);
  PPCContext native = ctx;
  FindNameInTable(native, base, tables);
  uint8_t native_out[4];
  std::memcpy(native_out, out_host, 4);
  std::memcpy(out_host, before, 4);
  original(ctx, base);
  if (native.r3.u32 != ctx.r3.u32 || std::memcmp(native_out, out_host, 4) != 0) {
    g_find_name_native = false;
    REXLOG_ERROR("bo1: native FindNameInTable differs from the game's (table {}, returned {:#x} "
                 "instead of {:#x}); using the game's",
                 ctx.r5.u32, native.r3.u32, ctx.r3.u32);
    return;
  }
  if (g_find_name_checks.fetch_sub(1, std::memory_order_relaxed) == 1) {
    REXLOG_INFO("bo1: native FindNameInTable matched the game's in {} calls", kFindNameChecks);
  }
}

}  // namespace

#endif

#if defined(BO1_BOOTSTRAP)
// No engine hooks.
#elif defined(BO1_MP)

REX_EXTERN(__imp__sub_82465CE8);
REX_HOOK_RAW(sub_82465CE8) {
  CheckedFindNameInTable(ctx, base, 0x83EE9380, __imp__sub_82465CE8);
}

REX_EXTERN(__imp__sub_82343D60);
REX_EXTERN(__imp__sub_8233E8D8);
REX_EXTERN(__imp__sub_82341CA8);

REX_HOOK_RAW(sub_82343D60) {
  bo1::engine::FrameStart(ctx, base, __imp__sub_8233E8D8);
  __imp__sub_82343D60(ctx, base);
}

REX_HOOK_RAW(sub_82341CA8) {
  bo1::engine::LogEngineError(ctx, base);
  __imp__sub_82341CA8(ctx, base);
}

#else

REX_EXTERN(__imp__sub_8242EF88);
REX_HOOK_RAW(sub_8242EF88) {
  CheckedFindNameInTable(ctx, base, 0x83A0F180, __imp__sub_8242EF88);
}

REX_EXTERN(__imp__sub_82315590);
REX_EXTERN(__imp__sub_8230FD58);
REX_EXTERN(__imp__sub_82313280);

REX_HOOK_RAW(sub_82315590) {
  bo1::engine::FrameStart(ctx, base, __imp__sub_8230FD58);
  __imp__sub_82315590(ctx, base);
}

REX_HOOK_RAW(sub_82313280) {
  bo1::engine::LogEngineError(ctx, base);
  __imp__sub_82313280(ctx, base);
}

#endif
