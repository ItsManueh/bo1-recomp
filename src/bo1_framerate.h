// bo1 - frame rate and frame pacing: a precise 60 FPS limiter, PC vsync and variable refresh rate

#pragma once

#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, bo1_frame_pacing);
REXCVAR_DECLARE(bool, bo1_vsync);
REXCVAR_DECLARE(bool, bo1_vrr);

namespace bo1::framerate {

// The game targets 60 frames per second, like the console. Multiplayer always uses it, so every
// player simulates movement at the same rate.
constexpr double kTargetFps = 60.0;

// Maps the options onto the presenter options. Call after the GPU plugin is loaded.
void ApplyRuntimeOptions();

// Engine commands (dvars) for the pacing mode; applied at startup and after every map change.
std::vector<std::string> EngineCommands();

// Called at the start of every game frame (Com_Frame) on the game main thread: waits until the
// next frame is due when the port limiter is active.
void WaitForNextFrame();

}  // namespace bo1::framerate
