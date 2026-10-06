// bo1 - graphics settings of the port
//
// Measured on a GTX 1070 in Kino der Toten (GPU time per frame): x1 ~12 ms, x2 ~28 ms, x3 ~58 ms.
// Most of the extra cost at higher resolutions is EDRAM depth/stencil transfers (the console keeps
// render targets in EDRAM; NVIDIA needs 8 stencil passes per transfer), so x1 is the default and
// the only setting that holds 60 FPS on that class of GPU; FSR upscaling sharpens its output.

#include "bo1_graphics.h"

#include <algorithm>

#include <fmt/format.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The game renders its 3D scene at 960x544 because the console's 10 MB of EDRAM holds no more; the
// port renders every draw at an integer multiple of it, so the HUD, menus, effects and render
// targets are all native at the chosen size (no upscaling of a small image).
REXCVAR_DEFINE_STRING(bo1_resolution, "console", "Black Ops",
                      "Rendering resolution: console (960x544, the original), 1080p (1920x1088), "
                      "1440p (2880x1632, shown downscaled on a 1440p screen), 4k (3840x2176) or "
                      "auto (the smallest of them that covers the monitor). Every step up needs "
                      "a much faster GPU for 60 FPS")
    .allowed({"console", "1080p", "1440p", "4k", "auto"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_upscaler, "fsr", "Black Ops",
                      "How the game image is scaled to the window: fsr (AMD FSR 1, sharp), cas "
                      "(contrast adaptive sharpening) or bilinear (like the console scaler)")
    .allowed({"fsr", "cas", "bilinear"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_aspect_ratio, "16:9", "Black Ops",
                      "Aspect ratio the game renders for, like the console display setting: 16:9 "
                      "(widescreen) or 4:3 (standard, narrower field of view)")
    .allowed({"16:9", "4:3"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bo1_stretch, false, "Black Ops",
                    "Stretch the image to fill the window instead of keeping its aspect ratio "
                    "with black bars")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_antialiasing, "smaa_ultra", "Black Ops",
                      "Edge smoothing of the final image (the game's own 2x/4x MSAA always stays "
                      "on): smaa_ultra (color edges, finds the most edges), smaa (brightness "
                      "edges), fxaa (cheapest, softer) or off. For a cleaner image still, raise "
                      "bo1_resolution")
    .allowed({"smaa_ultra", "smaa", "fxaa", "off"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_texture_filtering, "16x", "Black Ops",
                      "Anisotropic texture filtering: console (as the game asks) or 16x (sharper "
                      "ground and walls at grazing angles)")
    .allowed({"console", "16x"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_shadows, "high", "Black Ops",
                      "Shadow quality: console or high (full resolution spot light shadows and "
                      "sharper sun shadows near the player). Campaign only: multiplayer protects "
                      "those engine variables as cheats")
    .allowed({"console", "high"});

REXCVAR_DEFINE_STRING(bo1_lod, "high", "Black Ops",
                      "Model level of detail: console or high (detailed models are kept further "
                      "away, less pop-in)")
    .allowed({"console", "high"});

namespace bo1::graphics {

namespace {

constexpr int32_t kConsoleHeight = 544;

// Height in pixels of the primary monitor's current mode (physical pixels, whatever the DPI
// scaling); 0 if unknown.
int32_t PrimaryMonitorHeight() {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) return 0;
  return int32_t(mode.dmPelsHeight);
}

}  // namespace

int32_t ResolutionScale() {
  const std::string& option = REXCVAR_GET(bo1_resolution);
  if (option == "1080p") return 2;
  if (option == "1440p") return 3;
  if (option == "4k") return 4;
  if (option == "auto") {
    // Smallest multiple of 544 lines that covers the monitor (1080p -> 2, 1440p -> 3, 4K -> 4).
    const int32_t height = PrimaryMonitorHeight();
    if (height <= 0) return 1;
    return std::clamp((height + kConsoleHeight - 1) / kConsoleHeight, 1, 4);
  }
  return 1;
}

void ApplyRuntimeOptions() {
  const int32_t scale = ResolutionScale();
  for (const char* name : {"draw_resolution_scale_x", "draw_resolution_scale_y"}) {
    rex::cvar::SetFlagByName(name, fmt::format("{}", scale));
  }
  rex::cvar::SetFlagByName("present_effect", REXCVAR_GET(bo1_upscaler));
  // Antialiasing at presentation. MSAA is chosen by the game itself for each EDRAM render target
  // (2x or 4x, emulated natively) and cannot be raised; this smooths the edges of the final image.
  const std::string& antialiasing = REXCVAR_GET(bo1_antialiasing);
  rex::cvar::SetFlagByName("swap_post_effect", antialiasing == "smaa_ultra" ? "smaa"
                                               : antialiasing == "off"      ? "none"
                                                                            : antialiasing);
  rex::cvar::SetFlagByName("smaa_preset", antialiasing == "smaa_ultra" ? "ultra" : "high");
  // The game reads the display mode (VdQueryVideoMode) to choose widescreen or standard.
  const bool standard = REXCVAR_GET(bo1_aspect_ratio) == "4:3";
  rex::cvar::SetFlagByName("video_mode_width", standard ? "960" : "1280");
  rex::cvar::SetFlagByName("video_mode_height", "720");
  rex::cvar::SetFlagByName("present_letterbox", REXCVAR_GET(bo1_stretch) ? "false" : "true");
  // anisotropic_override: -1 = as the game asks, 5 = 16x.
  rex::cvar::SetFlagByName("anisotropic_override",
                           REXCVAR_GET(bo1_texture_filtering) == "16x" ? "5" : "-1");
  REXLOG_INFO("bo1: graphics: resolution {} = {}x{}, antialiasing {}, upscaler {}, aspect "
              "{}{}, textures {}, shadows {}, level of detail {}",
              REXCVAR_GET(bo1_resolution), 960 * scale, 544 * scale, antialiasing,
              REXCVAR_GET(bo1_upscaler),
              REXCVAR_GET(bo1_aspect_ratio),
              REXCVAR_GET(bo1_stretch) ? " stretched" : "", REXCVAR_GET(bo1_texture_filtering),
              REXCVAR_GET(bo1_shadows), REXCVAR_GET(bo1_lod));
}

std::vector<std::string> EngineCommands() {
  const bool high_shadows = REXCVAR_GET(bo1_shadows) == "high";
  const bool high_lod = REXCVAR_GET(bo1_lod) == "high";
  // Console values (Title Update #11) are restored when the option is set back to console.
  return {
      fmt::format("sm_fullResSpotShadowEnable {}", high_shadows ? 1 : 0),
      fmt::format("sm_sunSampleSizeNear {}", high_shadows ? "0.18" : "0.25"),
      // The LOD scale only goes from 1 to 4 (higher = less detail); a negative bias moves every
      // model's LOD switch further away instead (range -1000 .. 0, in world units).
      fmt::format("r_lodBiasRigid {}", high_lod ? -250 : 0),
      fmt::format("r_lodBiasSkinned {}", high_lod ? -250 : 0),
  };
}

}  // namespace bo1::graphics
