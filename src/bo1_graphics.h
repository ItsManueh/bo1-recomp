// bo1 - graphics settings of the port: internal resolution, upscaling, aspect ratio, texture
// filtering, shadows and level of detail

#pragma once

#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, bo1_resolution);
REXCVAR_DECLARE(std::string, bo1_upscaler);
REXCVAR_DECLARE(std::string, bo1_antialiasing);
REXCVAR_DECLARE(std::string, bo1_aspect_ratio);
REXCVAR_DECLARE(int32_t, bo1_fov);
REXCVAR_DECLARE(bool, bo1_stretch);
REXCVAR_DECLARE(std::string, bo1_texture_filtering);
REXCVAR_DECLARE(std::string, bo1_shadows);
REXCVAR_DECLARE(std::string, bo1_lod);

namespace bo1::graphics {

// Command that applies bo1_fov when the game has set cg_fov (now `current`) back to the console's
// 65 degrees; empty when there is nothing to do.
std::string FieldOfViewCommand(float current);

// Multiple of the console resolution (960x544) the game is rendered at, from bo1_resolution.
int32_t ResolutionScale();

// Maps the port options onto the runtime and GPU options. Call after the GPU plugin is loaded
// and bo1.toml has been read (they take effect when the graphics system starts).
void ApplyRuntimeOptions();

// Engine commands (dvars) for the current shadow and level of detail settings. The engine resets
// some of them when a map loads, so they are applied again after every map change.
std::vector<std::string> EngineCommands();

}  // namespace bo1::graphics
