// bo1 - screenshots of the game image

#include "bo1_capture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include "bo1_settings.h"

namespace bo1 {

namespace {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void WriteChunk(std::ofstream& file, const char type[4], const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> chunk;
  PutBE32(chunk, uint32_t(payload.size()));
  chunk.insert(chunk.end(), type, type + 4);
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  uint32_t crc = Crc32(chunk.data() + 4, chunk.size() - 4);
  PutBE32(chunk, crc);
  file.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
}

// 8-bit RGB PNG with zlib "stored" blocks (no compression): needs no library and any viewer
// opens it.
bool WritePng(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  file.write(reinterpret_cast<const char*>(kSignature), 8);

  std::vector<uint8_t> ihdr;
  PutBE32(ihdr, image.width);
  PutBE32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, deflate, filter 0, no interlacing
  WriteChunk(file, "IHDR", ihdr);

  // Uncompressed data: each row = filter byte 0 + RGB.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(image.height) * (1 + size_t(image.width) * 3));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      raw.push_back(row[x * 4 + 0]);
      raw.push_back(row[x * 4 + 1]);
      raw.push_back(row[x * 4 + 2]);
    }
  }
  std::vector<uint8_t> zlib = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || raw.empty();) {
    size_t len = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + len >= raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(uint8_t(len));
    zlib.push_back(uint8_t(len >> 8));
    zlib.push_back(uint8_t(~len));
    zlib.push_back(uint8_t(~len >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + len);
    pos += len;
    if (last) break;
  }
  PutBE32(zlib, (b << 16) | a);
  WriteChunk(file, "IDAT", zlib);
  WriteChunk(file, "IEND", {});
  return bool(file);
}

std::mutex g_timed_mutex;
std::condition_variable g_timed_cv;
bool g_timed_stop = false;
std::thread g_timed_thread;

}  // namespace

std::filesystem::path SaveGameScreenshot(rex::Runtime* runtime, const std::filesystem::path& dir,
                                         const std::string& name) {
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  if (!presenter) return {};
  rex::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
    REXLOG_WARN("bo1: no game image to capture yet");
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto path = dir / (name + ".png");
  if (!WritePng(path, image)) {
    REXLOG_ERROR("bo1: could not write screenshot {}", path.string());
    return {};
  }
  REXLOG_INFO("bo1: screenshot saved to {} ({}x{})", path.string(), image.width, image.height);
  return path;
}

void StartTimedCaptures(rex::Runtime* runtime, const std::filesystem::path& dir) {
  std::string spec = REXCVAR_GET(bo1_capture_at);
  if (spec.empty()) return;
  std::vector<int> seconds;
  std::stringstream ss(spec);
  for (std::string item; std::getline(ss, item, ',');) {
    try {
      seconds.push_back(std::stoi(item));
    } catch (...) {
    }
  }
  std::string prefix = REXCVAR_GET(bo1_capture_prefix);
  auto start = std::chrono::steady_clock::now();
  g_timed_stop = false;
  g_timed_thread = std::thread([runtime, dir, seconds, prefix, start] {
    for (int s : seconds) {
      std::unique_lock lock(g_timed_mutex);
      if (g_timed_cv.wait_until(lock, start + std::chrono::seconds(s), [] { return g_timed_stop; })) {
        return;
      }
      lock.unlock();
      SaveGameScreenshot(runtime, dir, prefix + "_" + std::to_string(s) + "s");
    }
  });
}

void StopTimedCaptures() {
  {
    std::lock_guard lock(g_timed_mutex);
    g_timed_stop = true;
  }
  g_timed_cv.notify_all();
  if (g_timed_thread.joinable()) g_timed_thread.join();
}

}  // namespace bo1
