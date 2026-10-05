// bo1 - profiles, saved games, backups and DLC

#include "bo1_storage.h"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <fmt/format.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace bo1 {

namespace {

constexpr const wchar_t* kFolderName = L"Call of Duty Black Ops (recompiled)";
// Folder name used by earlier builds; renamed to kFolderName once.
constexpr const wchar_t* kOldFolderName = L"Call of Duty Black Ops (recompilado)";
constexpr uint32_t kTitleId = 0x41560855;
constexpr uint32_t kContentTypeMarketplace = 0x00000002;  // DLC

// Folders that are not part of the save data: they regenerate by themselves or are copies.
bool SkipInBackup(const fs::path& rel) {
  for (const auto& part : rel) {
    auto p = part.string();
    if (p == "cache" || p == "cache_xbox" || p == "backups" || p == "DLC" || p == "00000002" ||
        p == "launch_data.bin") {
      return true;
    }
  }
  return false;
}

std::string Timestamp() {
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &now);
  return fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// STFS magic + content type (big-endian at 0x344) and title ID (0x360).
bool ReadStfsInfo(const fs::path& file, uint32_t& content_type, uint32_t& title_id) {
  std::ifstream in(file, std::ios::binary);
  char head[0x364];
  if (!in.read(head, sizeof(head))) return false;
  std::string magic(head, 4);
  if (magic != "CON " && magic != "LIVE" && magic != "PIRS") return false;
  auto be32 = [&](size_t o) {
    return (uint32_t(uint8_t(head[o])) << 24) | (uint32_t(uint8_t(head[o + 1])) << 16) |
           (uint32_t(uint8_t(head[o + 2])) << 8) | uint32_t(uint8_t(head[o + 3]));
  };
  content_type = be32(0x344);
  title_id = be32(0x360);
  return true;
}

}  // namespace

fs::path DefaultUserDataRoot() {
  PWSTR saved_games = nullptr;
  fs::path root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_CREATE, nullptr, &saved_games))) {
    root = fs::path(saved_games) / kFolderName;
    std::error_code ec;
    fs::path old_root = fs::path(saved_games) / kOldFolderName;
    if (!fs::exists(root, ec) && fs::is_directory(old_root, ec)) {
      fs::rename(old_root, root, ec);
    }
  }
  CoTaskMemFree(saved_games);
  return root;
}

fs::path BackupUserData(const fs::path& user_root, int keep) {
  std::error_code ec;
  if (keep <= 0 || !fs::exists(user_root, ec)) return {};
  const fs::path backups = user_root / "backups";
  const fs::path dest = backups / Timestamp();
  size_t files = 0;
  for (auto it = fs::recursive_directory_iterator(user_root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    fs::path rel = fs::relative(it->path(), user_root, ec);
    if (SkipInBackup(rel)) {
      if (it->is_directory()) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file()) continue;
    fs::create_directories((dest / rel).parent_path(), ec);
    fs::copy_file(it->path(), dest / rel, fs::copy_options::overwrite_existing, ec);
    if (!ec) ++files;
  }
  if (files == 0) {
    fs::remove_all(dest, ec);
    return {};
  }
  // Rotation: folders are named YYYYMMDD_HHMMSS, so alphabetical order is chronological.
  std::vector<fs::path> all;
  for (auto& e : fs::directory_iterator(backups, ec)) {
    if (e.is_directory()) all.push_back(e.path());
  }
  std::sort(all.begin(), all.end());
  while (all.size() > size_t(keep)) {
    fs::remove_all(all.front(), ec);
    all.erase(all.begin());
  }
  REXLOG_INFO("bo1: backed up {} files to {}", files, dest.string());
  return dest;
}

int InstallPendingDlc(rex::system::KernelState* kernel, const fs::path& dlc_dir) {
  std::error_code ec;
  fs::create_directories(dlc_dir, ec);
  if (!kernel || !kernel->content_manager()) return 0;
  int installed = 0;
  for (auto& entry : fs::directory_iterator(dlc_dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const fs::path& pkg = entry.path();
    if (pkg.extension() == ".installed" || pkg.extension() == ".instalado") continue;
    fs::path marker = pkg;
    marker += ".installed";
    fs::path old_marker = pkg;
    old_marker += ".instalado";  // marker written by earlier builds
    if (fs::exists(marker, ec) || fs::exists(old_marker, ec)) continue;
    uint32_t type = 0, title = 0;
    if (!ReadStfsInfo(pkg, type, title)) {
      REXLOG_WARN("bo1: {} is not an STFS package (CON/LIVE/PIRS), ignored",
                  pkg.filename().string());
      continue;
    }
    if (title != kTitleId || type != kContentTypeMarketplace) {
      REXLOG_WARN("bo1: {} is not Black Ops DLC (title ID {:08X}, type {:08X}), ignored",
                  pkg.filename().string(), title, type);
      continue;
    }
    rex::X_RESULT result = kernel->content_manager()->InstallContent(pkg);
    if (result == 0) {
      std::ofstream(marker) << "Installed by bo1 on " << Timestamp() << "\n";
      REXLOG_INFO("bo1: DLC installed: {}", pkg.filename().string());
      ++installed;
    } else {
      REXLOG_ERROR("bo1: could not install DLC {} (code {:08X})", pkg.filename().string(),
                   result);
    }
  }
  return installed;
}

std::vector<InstalledDlc> ListInstalledDlc(rex::system::KernelState* kernel) {
  std::vector<InstalledDlc> out;
  if (!kernel || !kernel->content_manager()) return out;
  // device_id 1 = hard drive; xuid 0 = content shared by all profiles.
  for (const auto& data : kernel->content_manager()->ListContent(
           1, 0, static_cast<rex::system::XContentType>(kContentTypeMarketplace), kTitleId)) {
    out.push_back({data.file_name(), data.display_name()});
  }
  return out;
}

bool MountCachePartition(rex::filesystem::VirtualFileSystem* vfs, const fs::path& dir) {
  if (!vfs) return false;
  std::error_code ec;
  fs::create_directories(dir, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\CACHE", dir, false, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("bo1: could not mount the cache partition at {}", dir.string());
    return false;
  }
  // Names the game and the console system use for that partition.
  vfs->RegisterSymbolicLink("cache:", "\\CACHE");
  vfs->RegisterSymbolicLink("\\Device\\Harddisk0\\Cache", "\\CACHE");
  vfs->RegisterSymbolicLink("\\Device\\cache", "\\CACHE");
  REXLOG_INFO("bo1: console cache partition mounted at {}", dir.string());
  return true;
}

std::string WindowsUserName() {
  wchar_t buf[257];
  DWORD len = static_cast<DWORD>(std::size(buf));
  std::string out;
  if (GetUserNameW(buf, &len)) {
    for (DWORD i = 0; i + 1 < len && out.size() < 15; ++i) {
      wchar_t c = buf[i];
      if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
          c == L' ' || c == L'_' || c == L'-') {
        out += static_cast<char>(c);
      }
    }
  }
  return out.empty() ? std::string("Player") : out;
}

}  // namespace bo1
