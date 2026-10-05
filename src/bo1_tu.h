// bo1 - Title Update: dump of the executable after the runtime has patched it

#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::system {
class KernelState;
}

namespace bo1 {

// Writes the loaded executable (header + in-memory image, after the runtime has applied
// update:\default.xexp) as an unencrypted, uncompressed XEX2. That file is the recompiler input
// that produces the game code with the Title Update.
bool DumpLoadedXex(rex::system::KernelState* kernel, const std::filesystem::path& out_path);

// Image size of the loaded executable (from its header, with the TU applied).
uint32_t LoadedImageSize(rex::system::KernelState* kernel);

}  // namespace bo1
