// bo1 - audio settings of the port: speaker layout, latency and subtitles

#pragma once

#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, bo1_audio_output);
REXCVAR_DECLARE(std::string, bo1_audio_latency);
REXCVAR_DECLARE(std::string, bo1_subtitles);

namespace bo1::audio {

// Maps the port options onto the runtime audio options. Call before the runtime is set up.
void ApplyRuntimeOptions();

// Engine commands (dvars) for the subtitle option; applied after every map change.
std::vector<std::string> EngineCommands();

// Current output latency estimate in milliseconds (0 before the first audio callback).
float OutputLatencyMs();

}  // namespace bo1::audio
