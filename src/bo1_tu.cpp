// bo1 - Title Update: dump of the executable after the runtime has patched it
//
// The recompiler (rexglue v0.10) does not apply .xexp patches, but the runtime does: when the
// game is loaded with update_data_root pointing at the extracted TU, XexModule::ApplyPatch patches
// the header and the image (LZX delta, like the console). This dumps the result as a "flat" XEX2.

#include "bo1_tu.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>

namespace bo1 {

namespace {

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

constexpr uint32_t kFileFormatInfoKey = 0x000003FF;
constexpr uint32_t kModulePatchFlags = 0x10 | 0x20 | 0x40;  // MODULE_PATCH | PATCH_FULL | PATCH_DELTA

}  // namespace

uint32_t LoadedImageSize(rex::system::KernelState* kernel) {
  if (!kernel) return 0;
  auto module = kernel->GetExecutableModule();
  if (!module || !module->xex_module()) return 0;
  const auto* raw = reinterpret_cast<const uint8_t*>(module->xex_module()->xex_header());
  return LoadBe32(raw + LoadBe32(raw + 0x10) + 4);
}

bool DumpLoadedXex(rex::system::KernelState* kernel, const std::filesystem::path& out_path) {
  if (!kernel) return false;
  auto module = kernel->GetExecutableModule();
  if (!module || !module->xex_module()) {
    REXLOG_ERROR("bo1: no executable module loaded to dump");
    return false;
  }
  auto* xex = module->xex_module();
  const auto* raw = reinterpret_cast<const uint8_t*>(xex->xex_header());
  const uint32_t header_size = LoadBe32(raw + 0x8);
  const uint32_t security_offset = LoadBe32(raw + 0x10);
  const uint32_t header_count = LoadBe32(raw + 0x14);
  const uint32_t image_size = LoadBe32(raw + security_offset + 4);

  std::vector<uint8_t> header(raw, raw + header_size);
  // A regular executable, not a patch.
  uint32_t flags = LoadBe32(header.data() + 4) & ~kModulePatchFlags;
  header[4] = uint8_t(flags >> 24);
  header[5] = uint8_t(flags >> 16);
  header[6] = uint8_t(flags >> 8);
  header[7] = uint8_t(flags);
  // No encryption (0) and no compression (0): the loader copies the image as is.
  bool format_patched = false;
  for (uint32_t i = 0; i < header_count; ++i) {
    const uint8_t* entry = header.data() + 0x18 + i * 8;
    if (LoadBe32(entry) == kFileFormatInfoKey) {
      uint32_t off = LoadBe32(entry + 4);
      StoreBe16(header.data() + off + 4, 0);  // encryption_type
      StoreBe16(header.data() + off + 6, 0);  // compression_type
      format_patched = true;
    }
  }
  if (!format_patched) {
    REXLOG_ERROR("bo1: the XEX header has no FILE_FORMAT_INFO");
    return false;
  }

  const uint8_t* loaded = kernel->memory()->TranslateVirtual<const uint8_t*>(xex->base_address());
  std::vector<uint8_t> image(loaded, loaded + image_size);

  // The loader has already replaced the records of imported variables (XboxHardwareInfo,
  // KeTimeStampBundle...) with runtime addresses. The original record is restored:
  //   (type << 24) | (library index << 16) | ordinal, type 0 = variable.
  // That way the file is identical to what the console would have after applying the TU.
  uint32_t restored = 0;
  const auto& libs = *xex->import_libraries();
  for (uint32_t lib = 0; lib < libs.size(); ++lib) {
    for (const auto& fn : libs[lib].imports) {
      if (fn.thunk_address != 0 || fn.value_address < xex->base_address()) continue;
      uint32_t off = fn.value_address - xex->base_address();
      if (off + 4 > image.size()) continue;
      uint32_t record = (lib << 16) | (fn.ordinal & 0xFFFF);
      image[off] = uint8_t(record >> 24);
      image[off + 1] = uint8_t(record >> 16);
      image[off + 2] = uint8_t(record >> 8);
      image[off + 3] = uint8_t(record);
      ++restored;
    }
  }

  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(header.data()), header.size());
  out.write(reinterpret_cast<const char*>(image.data()), image.size());
  REXLOG_INFO("bo1: {} imported variable records restored", restored);
  if (!out) {
    REXLOG_ERROR("bo1: could not write {}", out_path.string());
    return false;
  }
  REXLOG_INFO("bo1: loaded XEX dumped to {} (header {:#x} + image {:#x} from {:08X})",
              out_path.string(), header_size, image_size, xex->base_address());
  return true;
}

}  // namespace bo1
