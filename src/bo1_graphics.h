// bo1 - graphics settings of the port: internal resolution, upscaling, aspect ratio, texture
// filtering, shadows and level of detail

#pragma once

#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, bo1_internal_resolution);
REXCVAR_DECLARE(std::string, bo1_upscaler);
REXCVAR_DECLARE(std::string, bo1_antialiasing);
REXCVAR_DECLARE(std::string, bo1_aspect_ratio);
REXCVAR_DECLARE(bool, bo1_stretch);
REXCVAR_DECLARE(std::string, bo1_texture_filtering);
REXCVAR_DECLARE(std::string, bo1_shadows);
REXCVAR_DECLARE(std::string, bo1_lod);

namespace bo1::graphics {

// Maps the port options onto the runtime and GPU options. Call after the GPU plugin is loaded
// and bo1.toml has been read (they take effect when the graphics system starts).
void ApplyRuntimeOptions();

// Engine commands (dvars) for the current shadow and level of detail settings. The engine resets
// some of them when a map loads, so they are applied again after every map change.
std::vector<std::string> EngineCommands();

}  // namespace bo1::graphics
