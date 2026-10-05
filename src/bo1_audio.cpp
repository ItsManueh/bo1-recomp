// bo1 - audio settings of the port
//
// The console renders the game's sound as 5.1 (XAudio, 48 kHz, 256-sample frames); its XMA
// streams are decoded to PCM by the runtime (FFmpeg xmaframes decoder). The output stage delivers
// that 5.1 mix as stereo, headphones (virtual surround), 5.1 or 7.1 through SDL3 (WASAPI).

#include "bo1_audio.h"

#include <fmt/format.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bo1_audio_output, "auto", "Black Ops",
                      "Speakers: auto (follows the Windows playback device), stereo, headphones "
                      "(virtual surround for headphones: the 5.1 mix is placed around your head), "
                      "5.1 or 7.1")
    .allowed({"auto", "stereo", "headphones", "5.1", "7.1"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Measured with a 7.1 USB headset (WASAPI, 480-frame device buffer): normal = 53-57 ms from the
// game to the device, low = 31-36 ms. Normal never ran dry, not even during 5-second loading
// frames. Low had no gaps during gameplay but some (1-17 per 10 s) while the intro video played
// and a map loaded, when the CPU is busiest. The picture also reaches the screen a frame or two
// after the game draws it, so with normal the sound trails it by about 30 ms: below what can be
// noticed (lip sync errors start to show at about 45 ms with the sound early and over 100 ms with
// it late).
REXCVAR_DEFINE_STRING(bo1_audio_latency, "normal", "Black Ops",
                      "Audio buffering: normal (about 55 ms from the game to the speakers, the "
                      "safe choice) or low (about 35 ms; may crackle while loading on a busy PC)")
    .allowed({"normal", "low"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(bo1_subtitles, "game", "Black Ops",
                      "Subtitles in cinematics and dialogue: game (the in-game option decides), "
                      "on or off")
    .allowed({"game", "on", "off"});

namespace bo1::audio {

void ApplyRuntimeOptions() {
  rex::cvar::SetFlagByName("audio_output", REXCVAR_GET(bo1_audio_output));
  // Guest frames (5.33 ms each) the game may have queued ahead of the device.
  const bool low = REXCVAR_GET(bo1_audio_latency) == "low";
  rex::cvar::SetFlagByName("audio_maxqframes", low ? "4" : "8");
  REXLOG_INFO("bo1: audio: output {}, latency {}, subtitles {}", REXCVAR_GET(bo1_audio_output),
              REXCVAR_GET(bo1_audio_latency), REXCVAR_GET(bo1_subtitles));
}

std::vector<std::string> EngineCommands() {
  const std::string& subtitles = REXCVAR_GET(bo1_subtitles);
  if (subtitles == "game") return {};
  return {fmt::format("cg_subtitles {}", subtitles == "on" ? 1 : 0)};
}

float OutputLatencyMs() {
  return rex::audio::GetOutputLatencyMs();
}

}  // namespace bo1::audio
