// bo1 - frame rate and frame pacing
//
// On the console the game's r_vsync waits for the 60 Hz display. Under the emulator that wait is
// not tied to a real display: frames alternated between ~12 and ~22 ms while averaging 60 FPS,
// and with r_vsync 0 the game runs uncapped (~86 FPS). The port limiter replaces it: r_vsync is
// turned off and every Com_Frame starts exactly 1/60 s after the previous one. It sleeps with a
// high resolution waitable timer and spins only for the last fraction of a millisecond.

#include "bo1_framerate.h"

#include <atomic>

#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>

REXCVAR_DEFINE_STRING(bo1_frame_pacing, "port", "Black Ops",
                      "Frame pacing: port (precise 60 FPS limiter, every frame lasts 16.7 ms) or "
                      "console (the game's own vsync, uneven under emulation; campaign only)")
    .allowed({"port", "console"});

REXCVAR_DEFINE_BOOL(bo1_vsync, false, "Black Ops",
                    "Wait for the monitor refresh when presenting (no tearing, a little more "
                    "latency). Not needed with a variable refresh rate (G-Sync/FreeSync) display")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bo1_vrr, true, "Black Ops",
                    "Allow variable refresh rate (G-Sync/FreeSync) in borderless fullscreen when "
                    "vsync is off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace bo1::framerate {

namespace {

// Sleeping is precise to ~0.5 ms with a high resolution timer; the rest is spun.
constexpr double kSpinSeconds = 0.0008;

class Limiter {
 public:
  Limiter() {
    QueryPerformanceFrequency(&frequency_);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) {
      // Older Windows: a regular timer, precise to the 1 ms system timer resolution.
      timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
  }
  ~Limiter() {
    if (timer_) CloseHandle(timer_);
  }

  void Wait(double fps) {
    const int64_t period = int64_t(double(frequency_.QuadPart) / fps);
    const int64_t now = Now();
    if (next_ == 0 || now - next_ > period * 2) {
      // First frame, or far behind (a load or a hitch): restart the cadence instead of running
      // several frames back to back to catch up.
      next_ = now + period;
      return;
    }
    const int64_t spin = int64_t(double(frequency_.QuadPart) * kSpinSeconds);
    if (next_ - now > spin && timer_) {
      // Relative due time in 100 ns units (negative = relative).
      LARGE_INTEGER due;
      due.QuadPart = -((next_ - spin - now) * 10000000 / frequency_.QuadPart);
      if (due.QuadPart < 0 && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer_, INFINITE);
      }
    }
    while (Now() < next_) {
      _mm_pause();
    }
    next_ += period;
  }

 private:
  int64_t Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
  }

  LARGE_INTEGER frequency_{};
  HANDLE timer_ = nullptr;
  int64_t next_ = 0;
};

// Multiplayer always uses the port limiter: every player runs (and moves) at exactly 60 FPS, the
// common rate of the console, whatever the option says.
bool PortPacing() {
#if defined(BO1_MP)
  return true;
#else
  return REXCVAR_GET(bo1_frame_pacing) == "port";
#endif
}

}  // namespace

void ApplyRuntimeOptions() {
  rex::cvar::SetFlagByName("present_vsync", REXCVAR_GET(bo1_vsync) ? "true" : "false");
  rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing",
                           REXCVAR_GET(bo1_vrr) && !REXCVAR_GET(bo1_vsync) ? "true" : "false");
  REXLOG_INFO("bo1: frame pacing {}, vsync {}, variable refresh rate {}",
              REXCVAR_GET(bo1_frame_pacing), REXCVAR_GET(bo1_vsync) ? "on" : "off",
              REXCVAR_GET(bo1_vrr) && !REXCVAR_GET(bo1_vsync) ? "allowed" : "off");
}

std::vector<std::string> EngineCommands() {
  // With the port limiter the game must not wait on its own: r_vsync 1 under emulation is what
  // produced the uneven frame times.
  return {PortPacing() ? "r_vsync 0" : "r_vsync 1"};
}

void WaitForNextFrame() {
  if (!PortPacing()) return;
  static Limiter limiter;
  limiter.Wait(kTargetFps);
}

}  // namespace bo1::framerate
