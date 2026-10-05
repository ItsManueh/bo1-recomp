// bo1 - graphics settings of the port
//
// Measured on a GTX 1070 in Kino der Toten (GPU time per frame): x1 ~12 ms, x2 ~28 ms, x3 ~58 ms.
// Most of the extra cost at higher resolutions is EDRAM depth/stencil transfers (the console keeps
// render targets in EDRAM; NVIDIA needs 8 stencil passes per transfer), so x1 is the default and
// the only setting that holds 60 FPS on that class of GPU; FSR upscaling sharpens its output.

#include "bo1_graphics.h"

#include <fmt/format.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(bo1_internal_resolution, 1, "Black Ops",
                     "Internal rendering resolution as a multiple of the console one (960x544): "
                     "1 = original, 2 = 1920x1088, 3 = 2880x1632. The HUD, menus and every effect "
                     "are drawn at this resolution; 2 and 3 need a much faster GPU for 60 FPS")
    .range(1, 3)
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
                      "Edge smoothing of the final image (the game's own 2x/4x MSAA always stays on): "
                      "smaa_ultra (color edges, finds the most edges), smaa (brightness edges), "
                      "fxaa (cheapest, softer) or off. For a cleaner image still, raise "
                      "bo1_internal_resolution")
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

void ApplyRuntimeOptions() {
  const int32_t scale = REXCVAR_GET(bo1_internal_resolution);
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
  REXLOG_INFO("bo1: graphics: internal resolution {}x{}, antialiasing {}, upscaler {}, aspect "
              "{}{}, textures {}, shadows {}, level of detail {}",
              960 * scale, 544 * scale, antialiasing, REXCVAR_GET(bo1_upscaler),
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
