// bo1 - profiles, saved games, backups and DLC

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rex::system {
class KernelState;
}
namespace rex::filesystem {
class VirtualFileSystem;
}

namespace bo1 {

// Standard Windows folder for saved games:
//   %USERPROFILE%\Saved Games\Call of Duty Black Ops (recompiled)
// A folder left by older builds under the previous name is renamed to it once.
std::filesystem::path DefaultUserDataRoot();

// Copies the profile and saves to <root>\backups\YYYYMMDD_HHMMSS and keeps the 'keep' most recent.
// Returns the folder created (empty if there was nothing to copy).
std::filesystem::path BackupUserData(const std::filesystem::path& user_root, int keep);

// Installs, through the runtime ContentManager, the STFS packages (CON/LIVE/PIRS) found in
// <root>\DLC that are not installed yet. Returns how many were installed.
int InstallPendingDlc(rex::system::KernelState* kernel, const std::filesystem::path& dlc_dir);

struct InstalledDlc {
  std::string file_name;
  std::u16string display_name;
};
std::vector<InstalledDlc> ListInstalledDlc(rex::system::KernelState* kernel);

// Mounts the Xbox 360 hard drive cache partition ("cache:", "\Device\Harddisk0\Cache") on a PC
// folder. Multiplayer uses it as scratch storage; without it the runtime answers
// "ResolvePath(cache:\) failed".
bool MountCachePartition(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& dir);

// Windows user name adapted to a gamertag (ASCII, at most 15 characters).
std::string WindowsUserName();

}  // namespace bo1
