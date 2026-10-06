// Sampling profiler for bo1.exe.
//
// 1) Measures 1 s of CPU time per thread and keeps the threads that use CPU.
// 2) Periodically suspends those threads, copies their stack and walks it (StackWalk64).
// 3) Resolves the addresses with the PDB (dbghelp) and aggregates by:
//    - function at the top of the stack,
//    - first ReXGlue runtime function (rex*.dll / non-generated bo1.exe code) on the stack,
//    - first recompiled game function (sub_XXXXXXXX) on the stack.
//
// Usage: sampler.exe <pid> [seconds=10] [interval_ms=1] [cpu_threshold_pct=5] [threads] [samples.tsv]
//   threads: name parts separated by commas ("-" = all); only those threads are sampled and their
//            most frequent full stacks are printed.
//   samples.tsv: saves every sample (time in ms since 1970, thread, stack) to cross it with the
//            hitches in the game log (tools/stutter_report.py).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kMaxFrames = 24;

std::vector<DWORD> ListThreads(DWORD pid) {
  std::vector<DWORD> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID == pid) out.push_back(te.th32ThreadID);
  }
  CloseHandle(snap);
  return out;
}

std::string ThreadName(HANDLE h) {
  PWSTR desc = nullptr;
  std::string s;
  if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
    int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr);
    s.resize(n > 0 ? n - 1 : 0);
    WideCharToMultiByte(CP_UTF8, 0, desc, -1, s.data(), n, nullptr, nullptr);
    LocalFree(desc);
  }
  return s;
}

uint64_t CpuTime100ns(HANDLE h) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
         (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

struct Stack {
  DWORD64 pc[kMaxFrames];
  int n = 0;
  double time_ms = 0;  // system time, ms since 1970
};

double NowEpochMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
  return double(t - 116444736000000000ull) / 10000.0;
}

// Copy of the sampled thread stack: the thread is only suspended while it is copied; the stack is
// walked afterwards, reading from the copy.
struct StackCopy {
  HANDLE process = nullptr;
  DWORD64 base = 0;
  std::vector<uint8_t> bytes;
};
StackCopy g_copy;

BOOL CALLBACK ReadFromCopy(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
  if (address >= g_copy.base && address + size <= g_copy.base + g_copy.bytes.size()) {
    std::memcpy(buffer, g_copy.bytes.data() + (address - g_copy.base), size);
    if (read) *read = size;
    return TRUE;
  }
  SIZE_T done = 0;
  BOOL ok = ReadProcessMemory(g_copy.process, LPCVOID(address), buffer, size, &done);
  if (read) *read = DWORD(done);
  return ok;
}

// Highest address of the thread stack (NT_TIB::StackBase of its TEB).
DWORD64 StackBase(HANDLE process, HANDLE thread) {
  struct {
    LONG exit_status;
    PVOID teb;
    PVOID client_id[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
  } info{};
  using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static auto query = reinterpret_cast<QueryFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  DWORD64 base = 0;
  if (query && query(thread, 0, &info, sizeof(info), nullptr) >= 0 && info.teb) {
    ReadProcessMemory(process, static_cast<uint8_t*>(info.teb) + 8, &base, sizeof(base), nullptr);
  }
  return base;
}

void Print(const char* title, std::map<std::string, uint32_t>& m, uint64_t total, size_t top) {
  std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::printf("\n== %s ==\n", title);
  for (size_t i = 0; i < std::min(top, v.size()); ++i) {
    std::printf("%6.2f%%  %s\n", 100.0 * v[i].second / total, v[i].first.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: sampler <pid> [seconds] [interval_ms] [cpu_threshold_pct] [threads] [samples.tsv]\n");
    return 1;
  }
  DWORD pid = std::strtoul(argv[1], nullptr, 10);
  int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
  int interval_ms = argc > 3 ? std::atoi(argv[3]) : 1;
  double threshold = argc > 4 ? std::atof(argv[4]) : 5.0;
  std::vector<std::string> only_threads;
  if (argc > 5 && std::string(argv[5]) != "-") {
    std::string list = argv[5];
    for (size_t start = 0; start <= list.size();) {
      size_t comma = std::min(list.find(',', start), list.size());
      if (comma > start) only_threads.push_back(list.substr(start, comma - start));
      start = comma + 1;
    }
  }
  auto selected = [&](const std::string& name) {
    for (const std::string& part : only_threads) {
      if (name.find(part) != std::string::npos) return true;
    }
    return false;
  };
  const char* raw_path = argc > 6 ? argv[6] : nullptr;

  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) {
    std::fprintf(stderr, "could not open process %lu\n", pid);
    return 1;
  }
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);

  struct ThreadInfo {
    HANDLE handle = nullptr;
    DWORD64 stack_base = 0;
    std::string name;
    double cpu_pct = 0;
    std::vector<Stack> stacks;
  };
  std::map<DWORD, ThreadInfo> threads;

  // 1) thread selection by CPU usage
  std::map<DWORD, uint64_t> t0;
  for (DWORD tid : ListThreads(pid)) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, tid);
    if (!h) continue;
    threads[tid].handle = h;
    threads[tid].name = ThreadName(h);
    t0[tid] = CpuTime100ns(h);
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::vector<DWORD> hot;
  for (auto& [tid, ti] : threads) {
    ti.cpu_pct = (CpuTime100ns(ti.handle) - t0[tid]) / 1e5;  // 100 ns units in 1 s -> %
    if (ti.cpu_pct >= threshold && (only_threads.empty() || selected(ti.name))) hot.push_back(tid);
  }
  for (DWORD tid : hot) threads[tid].stack_base = StackBase(process, threads[tid].handle);
  g_copy.process = process;
  std::printf("Threads with >= %.0f%% CPU: %zu\n", threshold, hot.size());
  for (DWORD tid : hot) {
    std::printf("  %6lu  %5.1f%%  %s\n", tid, threads[tid].cpu_pct, threads[tid].name.c_str());
  }

  // 2) stack sampling
  timeBeginPeriod(1);
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  uint64_t rounds = 0;
  while (std::chrono::steady_clock::now() < end) {
    for (DWORD tid : hot) {
      auto& ti = threads[tid];
      if (SuspendThread(ti.handle) == DWORD(-1)) continue;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      bool have_context = GetThreadContext(ti.handle, &ctx);
      if (have_context) {
        constexpr DWORD64 kMaxCopy = 512 * 1024;
        DWORD64 top = ti.stack_base > ctx.Rsp ? std::min(ti.stack_base, ctx.Rsp + kMaxCopy)
                                              : ctx.Rsp + 64 * 1024;
        g_copy.base = ctx.Rsp;
        g_copy.bytes.resize(size_t(top - ctx.Rsp));
        SIZE_T done = 0;
        ReadProcessMemory(process, LPCVOID(ctx.Rsp), g_copy.bytes.data(), g_copy.bytes.size(),
                          &done);
        g_copy.bytes.resize(done);
      }
      ResumeThread(ti.handle);
      if (have_context) {
        Stack st;
        st.time_ms = NowEpochMs();
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        while (st.n < kMaxFrames &&
               StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, ti.handle, &frame, &ctx, ReadFromCopy,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
               frame.AddrPC.Offset) {
          st.pc[st.n++] = frame.AddrPC.Offset;
        }
        ti.stacks.push_back(st);
      }
    }
    ++rounds;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  timeEndPeriod(1);

  // 3) symbols and aggregates
  std::unordered_map<DWORD64, std::string> cache;
  auto resolve = [&](DWORD64 addr) -> const std::string& {
    auto it = cache.find(addr);
    if (it != cache.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mod{sizeof(mod)};
    std::string module = SymGetModuleInfo64(process, addr, &mod) ? mod.ModuleName : "?";
    std::string name = SymFromAddr(process, addr, &disp, sym) ? module + "!" + sym->Name
                                                              : module + "!?";
    return cache.emplace(addr, std::move(name)).first->second;
  };
  auto is_os = [](const std::string& s) {
    return s.starts_with("ntdll!") || s.starts_with("KERNELBASE!") ||
           s.starts_with("kernel32!") || s.starts_with("KERNEL32!") ||
           s.starts_with("MSVCP140!") || s.starts_with("VCRUNTIME140!") ||
           s.starts_with("ucrtbase!") || s.starts_with("win32u!");
  };
  auto is_guest = [](const std::string& s) {
    return s.find("!__imp__sub_") != std::string::npos || s.find("!sub_8") != std::string::npos;
  };

  // Source line of an address ("file:line"), for the hottest top-of-stack lines.
  std::unordered_map<DWORD64, std::string> line_cache;
  auto resolve_line = [&](DWORD64 addr) -> const std::string& {
    auto it = line_cache.find(addr);
    if (it != line_cache.end()) return it->second;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD disp = 0;
    std::string text = resolve(addr);
    if (SymGetLineFromAddr64(process, addr, &disp, &line) && line.FileName) {
      std::string file = line.FileName;
      size_t slash = file.find_last_of("\\/");
      if (slash != std::string::npos) file = file.substr(slash + 1);
      text += " (" + file + ":" + std::to_string(line.LineNumber) + ")";
    }
    return line_cache.emplace(addr, std::move(text)).first->second;
  };

  uint64_t total = 0;
  std::map<std::string, uint32_t> by_top, by_runtime, by_guest, by_pair, by_line, inclusive;
  std::printf("\nRounds: %llu in %d s\n", (unsigned long long)rounds, seconds);
  for (DWORD tid : hot) {
    auto& ti = threads[tid];
    std::map<std::string, uint32_t> t_runtime, t_guest;
    for (auto& st : ti.stacks) {
      if (!st.n) continue;
      ++total;
      by_top[resolve(st.pc[0])]++;
      by_line[resolve_line(st.pc[0])]++;
      // Inclusive: every function on the stack, once per sample.
      std::vector<const std::string*> seen;
      for (int i = 0; i < st.n; ++i) {
        const std::string& f = resolve(st.pc[i]);
        if (std::find(seen.begin(), seen.end(), &f) == seen.end()) {
          seen.push_back(&f);
          inclusive[f]++;
        }
      }
      std::string first_runtime, first_guest;
      for (int i = 0; i < st.n; ++i) {
        const auto& f = resolve(st.pc[i]);
        if (first_runtime.empty() && !is_os(f) && !is_guest(f)) first_runtime = f;
        if (first_guest.empty() && is_guest(f)) {
          first_guest = f;
          break;
        }
      }
      if (first_runtime.empty()) first_runtime = "(OS only)";
      if (first_guest.empty()) first_guest = "(no game code on the stack)";
      by_runtime[first_runtime]++;
      by_guest[first_guest]++;
      by_pair[first_guest + "  ->  " + first_runtime]++;
      t_runtime[first_runtime]++;
      t_guest[first_guest]++;
    }
    std::printf("\n-- thread %lu '%s' (%.1f%% CPU, %zu samples)\n", tid, ti.name.c_str(), ti.cpu_pct,
                ti.stacks.size());
    std::vector<std::pair<std::string, uint32_t>> v(t_runtime.begin(), t_runtime.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(5, v.size()); ++i)
      std::printf("   runtime %5.1f%%  %s\n", 100.0 * v[i].second / ti.stacks.size(),
                  v[i].first.c_str());
    std::vector<std::pair<std::string, uint32_t>> g(t_guest.begin(), t_guest.end());
    std::sort(g.begin(), g.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(5, g.size()); ++i)
      std::printf("   game    %5.1f%%  %s\n", 100.0 * g[i].second / ti.stacks.size(),
                  g[i].first.c_str());
  }
  // Short stack: consecutive frames of the same OS or driver module are merged into one.
  auto describe = [&](const Stack& st, int max_shown, const char* separator) {
    std::string key, prev_module;
    int shown = 0;
    for (int i = 0; i < st.n && shown < max_shown; ++i) {
      const std::string& f = resolve(st.pc[i]);
      std::string module = f.substr(0, f.find('!'));
      bool foreign = module != "rexgpu-xenosrd" && module != "rexruntimerd" && module != "bo1";
      if (foreign && module == prev_module) continue;
      prev_module = module;
      if (shown++) key += separator;
      key += f;
    }
    return key;
  };
  if (raw_path) {
    FILE* raw = nullptr;
    if (fopen_s(&raw, raw_path, "w") == 0 && raw) {
      for (DWORD tid : hot) {
        for (const Stack& st : threads[tid].stacks) {
          std::fprintf(raw, "%.3f\t%s\t%s\n", st.time_ms, threads[tid].name.c_str(),
                       describe(st, 16, " < ").c_str());
        }
      }
      std::fclose(raw);
    }
  }
  if (!only_threads.empty()) {
    for (DWORD tid : hot) {
      auto& ti = threads[tid];
      std::map<std::string, uint32_t> full;
      for (auto& st : ti.stacks) {
        full[describe(st, 12, "\n        <- ")]++;
      }
      Print(("Full stacks of '" + ti.name + "'").c_str(), full, ti.stacks.size(), 25);
    }
  }
  Print("Top of the stack (all hot threads)", by_top, total, 25);
  Print("Top of the stack by source line", by_line, total, 40);
  Print("Inclusive (function anywhere on the stack)", inclusive, total, 60);
  Print("First runtime function on the stack", by_runtime, total, 25);
  Print("First game function on the stack", by_guest, total, 25);
  Print("Game -> runtime pairs", by_pair, total, 30);
  return 0;
}
