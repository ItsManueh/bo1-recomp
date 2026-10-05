// bo1 - HLE kernel improvements (implemented in bo1_kernel.cpp)

#pragma once

#include <filesystem>

namespace bo1 {

// File through which XamLoaderSetLaunchData/GetLaunchData pass data between the campaign
// (bo1.exe) and multiplayer (bo1mp.exe). Data older than 2 minutes is discarded.
void SetLaunchDataFile(const std::filesystem::path& file);

}  // namespace bo1
