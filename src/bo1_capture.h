// bo1 - screenshots of the game image (independent of the graphics backend)

#pragma once

#include <filesystem>
#include <string>

namespace rex {
class Runtime;
}

namespace bo1 {

// Saves the last image the game presented as a PNG (at its internal resolution, before scaling to
// the window). Returns the path written, or an empty path if there is no image yet.
std::filesystem::path SaveGameScreenshot(rex::Runtime* runtime, const std::filesystem::path& dir,
                                         const std::string& name);

// Automated tests: with bo1_capture_at = "40,65" saves screenshots at those seconds after startup
// to <dir>\<prefix>_<seconds>s.png. Does nothing if the option is empty.
void StartTimedCaptures(rex::Runtime* runtime, const std::filesystem::path& dir);
void StopTimedCaptures();

}  // namespace bo1
