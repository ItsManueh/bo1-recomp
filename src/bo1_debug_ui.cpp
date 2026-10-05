// bo1 - in-game developer UI: the developer console window (F1) and the compact overlay (F2)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "bo1_audio.h"
#include "bo1_debug.h"
#include "bo1_engine.h"
#include "bo1_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bo1::debug {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_console_open{false};
std::atomic<bool> g_overlay_visible{true};

// --- Look ------------------------------------------------------------------------------------------

const ImVec4 kAccent(0.91f, 0.58f, 0.18f, 1.0f);
const ImVec4 kAccentDim(0.55f, 0.34f, 0.10f, 1.0f);
const ImVec4 kGood(0.45f, 0.95f, 0.45f, 1.0f);
const ImVec4 kFair(1.0f, 0.82f, 0.30f, 1.0f);
const ImVec4 kBad(1.0f, 0.40f, 0.40f, 1.0f);
const ImVec4 kMuted(0.60f, 0.60f, 0.62f, 1.0f);

ImVec4 LevelColor(Level level) {
  switch (level) {
    case Level::kTrace: return ImVec4(0.50f, 0.50f, 0.52f, 1.0f);
    case Level::kDebug: return ImVec4(0.45f, 0.75f, 0.90f, 1.0f);
    case Level::kInfo: return ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    case Level::kWarn: return kFair;
    default: return kBad;
  }
}

const char* LevelName(Level level) {
  static const char* kNames[] = {"trace", "debug", "info", "warn", "error"};
  return kNames[size_t(level)];
}

ImVec4 FpsColor(double fps) { return fps >= 58.0 ? kGood : fps >= 45.0 ? kFair : kBad; }

// Theme for the developer windows, pushed around them only (the game overlay has its own).
class ScopedTheme {
 public:
  ScopedTheme() {
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 0.96f)},
        {ImGuiCol_ChildBg, ImVec4(0.03f, 0.03f, 0.04f, 0.85f)},
        {ImGuiCol_Border, ImVec4(0.30f, 0.22f, 0.12f, 0.80f)},
        {ImGuiCol_TitleBg, ImVec4(0.10f, 0.08f, 0.06f, 1.0f)},
        {ImGuiCol_TitleBgActive, kAccentDim},
        {ImGuiCol_Tab, ImVec4(0.14f, 0.12f, 0.10f, 1.0f)},
        {ImGuiCol_TabHovered, kAccent},
        {ImGuiCol_TabSelected, kAccentDim},
        {ImGuiCol_Header, ImVec4(0.30f, 0.20f, 0.08f, 0.80f)},
        {ImGuiCol_HeaderHovered, ImVec4(0.45f, 0.30f, 0.10f, 0.90f)},
        {ImGuiCol_HeaderActive, kAccentDim},
        {ImGuiCol_Button, ImVec4(0.22f, 0.17f, 0.11f, 1.0f)},
        {ImGuiCol_ButtonHovered, kAccentDim},
        {ImGuiCol_ButtonActive, kAccent},
        {ImGuiCol_FrameBg, ImVec4(0.13f, 0.12f, 0.11f, 1.0f)},
        {ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.17f, 0.13f, 1.0f)},
        {ImGuiCol_CheckMark, kAccent},
        {ImGuiCol_SliderGrab, kAccent},
        {ImGuiCol_PlotLines, kAccent},
        {ImGuiCol_PlotHistogram, kAccent},
        {ImGuiCol_TableHeaderBg, ImVec4(0.16f, 0.13f, 0.10f, 1.0f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.03f)},
        {ImGuiCol_Separator, ImVec4(0.30f, 0.22f, 0.12f, 0.80f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0.03f, 0.03f, 0.04f, 0.60f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.35f, 0.25f, 0.13f, 1.0f)},
        {ImGuiCol_ScrollbarGrabHovered, kAccentDim},
        {ImGuiCol_ScrollbarGrabActive, kAccent},
        {ImGuiCol_ResizeGrip, ImVec4(0.35f, 0.25f, 0.13f, 0.6f)},
        {ImGuiCol_ResizeGripHovered, kAccentDim},
        {ImGuiCol_ResizeGripActive, kAccent},
        {ImGuiCol_TextSelectedBg, ImVec4(0.55f, 0.34f, 0.10f, 0.6f)},
    };
    for (const auto& [index, color] : colors) ImGui::PushStyleColor(index, color);
    color_count_ = int(std::size(colors));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  }
  ~ScopedTheme() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(color_count_);
  }

 private:
  int color_count_ = 0;
};

void CopyToClipboard(const std::string& text) {
  int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()) + 1, nullptr, 0);
  if (count <= 0 || !OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size_t(count) * sizeof(wchar_t))) {
    if (auto* dst = static_cast<wchar_t*>(GlobalLock(memory))) {
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()) + 1, dst, count);
      GlobalUnlock(memory);
      if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

bool ContainsNoCase(const std::string& haystack, const char* needle) {
  if (!*needle) return true;
  auto it = std::search(haystack.begin(), haystack.end(), needle, needle + std::strlen(needle),
                        [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
  return it != haystack.end();
}

bool StartsWithNoCase(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         _strnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// Developer toggles: engine dvars shown as checkboxes in the Engine tab.
struct Toggle {
  const char* dvar;
  const char* label;
};
constexpr Toggle kToggles[] = {
    {"developer", "Engine developer mode (retail game may fail checks)"},
    {"developer_script", "Script asserts (retail scripts fail them)"},
    {"cg_drawFPS", "Engine FPS counter"},
    {"cg_drawPerformanceWarnings", "Engine performance warnings"},
    {"con_minicon", "Engine mini console"},
    {"cg_drawVersion", "Build version"},
    {"cg_drawSnapshot", "Network snapshot info"},
    {"cg_drawLagometer", "Lagometer"},
    {"cg_drawHUD", "HUD"},
    {"cg_drawGun", "Weapon (viewmodel)"},
};

constexpr const char* kTabNames[] = {"Console", "Performance", "Loading", "Engine", "Dvars"};

// Engine state shown in the Engine tab.
constexpr const char* kWatchedDvars[] = {
    "mapname",  "g_gametype", "sv_running", "cl_paused", "com_maxfps", "r_vsync",
    "cg_fov",   "sv_fps",     "developer",  "developer_script", "r_lodScaleRigid", "sm_enable",
    "splitscreen", "splitscreen_playerCount", "r_num_viewports", "cg_subtitles",
    "snd_speakerConfiguration",
};

// --- Window ----------------------------------------------------------------------------------------

class DeveloperUi final : public rex::ui::ImGuiDialog {
 public:
  DeveloperUi(rex::ui::ImGuiDrawer* drawer, const char* title)
      : ImGuiDialog(drawer), title_(title) {
    g_overlay_visible = REXCVAR_GET(bo1_overlay);
    min_level_ = LevelFromName(REXCVAR_GET(bo1_console_level));
  }

  // Positions the window and focuses the command line the next time it is drawn, showing `tab`
  // (index in kTabNames, or -1 to keep the current one).
  void MarkOpened(int tab) {
    just_opened_ = true;
    select_tab_ = tab;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    PollLog();
    if (g_overlay_visible && !g_console_open) DrawOverlay();
    if (g_console_open) {
      ScopedTheme theme;
      DrawConsole(io);
    }
  }

 private:
  static int LevelFromName(const std::string& name) {
    for (int i = 0; i <= int(Level::kError); ++i) {
      if (name == LevelName(Level(i))) return i;
    }
    return int(Level::kInfo);
  }

  void PollLog() {
    size_t before = lines_.size();
    next_sequence_ = CopyLogSince(next_sequence_, lines_);
    if (lines_.size() > kMaxLines) {
      lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
    }
    if (lines_.size() != before && auto_scroll_) scroll_to_bottom_ = true;
  }

  // --- Compact overlay (F2) ------------------------------------------------------------------------

  void DrawOverlay() {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##bo1_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    FrameStats frames = GetFrameStats();
    ImGui::TextColored(FpsColor(frames.fps), "%.0f FPS", frames.fps);
    ImGui::SameLine();
    ImGui::Text("%.1f ms  jitter %.2f  worst %.1f  1%% low %.0f", frames.avg_ms,
                frames.jitter_ms, frames.worst_ms, frames.low_1pct_fps);
    ImGui::PlotLines("##frames", frames.history.data(), int(frames.history.size()), 0, nullptr,
                     0.0f, 50.0f, ImVec2(310, 34));
    if (REXCVAR_GET(bo1_dev_mode)) {
      RefreshWatched();
      ImGui::TextDisabled("map %s   frame %llu", watched_map_.c_str(),
                          static_cast<unsigned long long>(engine::FrameCount()));
    }
    LoadStats loads = GetLoadStats();
    bool loading = false;
    for (const auto& file : loads.open) {
      if (file.seconds < 1.5) {
        if (!loading) ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Loading (%.1f MB/s):",
                                         loads.read_rate_mbs);
        loading = true;
        ImGui::Text("  %s", file.path.c_str());
      }
    }
    for (const auto& [text, error] : GetNotices()) {
      ImGui::TextColored(error ? kBad : kFair, "%s", text.c_str());
    }
    ImGui::TextDisabled("F1 developer console   F2 hide");
    ImGui::End();
  }

  // --- Developer console (F1) ----------------------------------------------------------------------

  void DrawConsole(ImGuiIO& io) {
    if (just_opened_) {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.05f, io.DisplaySize.y * 0.05f),
                              ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.9f, io.DisplaySize.y * 0.75f),
                               ImGuiCond_Appearing);
    }
    bool open = true;
    std::string window_title = std::string(title_) + " - Developer Console###bo1_console";
    if (!ImGui::Begin(window_title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) g_console_open = false;
    DrawHeader();
    using DrawTab = void (DeveloperUi::*)();
    static const DrawTab kDrawTabs[] = {&DeveloperUi::DrawConsoleTab,
                                        &DeveloperUi::DrawPerformanceTab,
                                        &DeveloperUi::DrawLoadingTab, &DeveloperUi::DrawEngineTab,
                                        &DeveloperUi::DrawDvarsTab};
    static_assert(std::size(kDrawTabs) == std::size(kTabNames));
    if (ImGui::BeginTabBar("##tabs")) {
      for (int i = 0; i < int(std::size(kTabNames)); ++i) {
        ImGuiTabItemFlags flags = select_tab_ == i ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(kTabNames[i], nullptr, flags)) {
          (this->*kDrawTabs[i])();
          ImGui::EndTabItem();
        }
      }
      select_tab_ = -1;
      ImGui::EndTabBar();
    }
    ImGui::End();
    just_opened_ = false;
  }

  void DrawHeader() {
    FrameStats frames = GetFrameStats();
    RefreshWatched();
    ImGui::TextColored(FpsColor(frames.fps), "%.0f FPS", frames.fps);
    ImGui::SameLine();
    ImGui::TextDisabled("| %.2f ms | map %s | %s | frame %llu", frames.avg_ms,
                        watched_map_.c_str(),
#if defined(BO1_MP)
                        "multiplayer",
#else
                        "campaign",
#endif
                        static_cast<unsigned long long>(engine::FrameCount()));
    ImGui::SameLine(ImGui::GetWindowWidth() - 190);
    ImGui::TextDisabled("F1 / Esc closes");
  }

  // Console tab: the log with filters and the command line.
  void DrawConsoleTab() {
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filter", "Filter text", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    const char* levels[] = {"trace", "debug", "info", "warn", "error"};
    ImGui::Combo("##level", &min_level_, levels, IM_ARRAYSIZE(levels));
    ImGui::SameLine();
    ImGui::Checkbox("Port only", &port_only_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
      lines_.clear();
    }
    ImGui::SameLine();
    bool copy = ImGui::Button("Copy");

    const float footer = ImGui::GetFrameHeightWithSpacing() * (suggestions_.empty() ? 1.3f : 2.4f);
    ImGui::BeginChild("##log", ImVec2(0, -footer), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    visible_.clear();
    for (size_t i = 0; i < lines_.size(); ++i) {
      const LogLine& line = lines_[i];
      if (int(line.level) < min_level_) continue;
      if (port_only_ && line.text.rfind("bo1:", 0) != 0) continue;
      if (filter_[0] && !ContainsNoCase(line.text, filter_)) continue;
      visible_.push_back(i);
    }
    std::string copied;
    ImGuiListClipper clipper;
    clipper.Begin(int(visible_.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const LogLine& line = lines_[visible_[size_t(row)]];
        ImGui::TextDisabled("%s", line.time.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%-4s", line.category.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(line.level));
        ImGui::TextUnformatted(line.text.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (copy) {
      for (size_t index : visible_) {
        const LogLine& line = lines_[index];
        copied += line.time + " [" + LevelName(line.level) + "] " + line.text + "\n";
      }
      CopyToClipboard(copied);
    }
    if (scroll_to_bottom_) ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
    ImGui::EndChild();

    DrawCommandLine();
  }

  void DrawCommandLine() {
    if (just_opened_ || refocus_input_) {
      ImGui::SetKeyboardFocusHere();
      refocus_input_ = false;
    }
    ImGui::SetNextItemWidth(-80);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory |
                                      ImGuiInputTextFlags_CallbackCompletion |
                                      ImGuiInputTextFlags_CallbackEdit;
    bool submit = ImGui::InputTextWithHint(
        "##command", "Command or dvar ('help' lists the commands; Tab completes, Up/Down history)",
        input_, sizeof(input_), flags, &DeveloperUi::InputCallback, this);
    ImGui::SameLine();
    submit |= ImGui::Button("Run", ImVec2(70, 0));
    if (submit && input_[0]) {
      std::string line = input_;
      if (history_.empty() || history_.back() != line) history_.push_back(line);
      history_pos_ = -1;
      input_[0] = '\0';
      suggestions_.clear();
      if (line == "clear" || line == "cls") {
        lines_.clear();
      } else {
        RunCommand(line);
      }
      auto_scroll_ = true;
      refocus_input_ = true;
    }
    if (!suggestions_.empty()) {
      std::string text;
      for (const auto& suggestion : suggestions_) {
        text += (text.empty() ? "" : "   ") + suggestion;
      }
      ImGui::TextColored(kMuted, "%s", text.c_str());
    }
  }

  static int InputCallback(ImGuiInputTextCallbackData* data) {
    return static_cast<DeveloperUi*>(data->UserData)->OnInput(data);
  }

  int OnInput(ImGuiInputTextCallbackData* data) {
    switch (data->EventFlag) {
      case ImGuiInputTextFlags_CallbackHistory: {
        if (history_.empty()) break;
        int previous = history_pos_;
        if (data->EventKey == ImGuiKey_UpArrow) {
          history_pos_ = history_pos_ < 0 ? int(history_.size()) - 1 : std::max(0, history_pos_ - 1);
        } else if (data->EventKey == ImGuiKey_DownArrow && history_pos_ >= 0) {
          history_pos_ = history_pos_ + 1 >= int(history_.size()) ? -1 : history_pos_ + 1;
        }
        if (previous != history_pos_) {
          data->DeleteChars(0, data->BufTextLen);
          if (history_pos_ >= 0) data->InsertChars(0, history_[size_t(history_pos_)].c_str());
        }
        break;
      }
      case ImGuiInputTextFlags_CallbackCompletion: {
        std::string word(data->Buf, size_t(data->BufTextLen));
        if (word.empty() || word.find(' ') != std::string::npos) break;
        std::vector<std::string> matches = Matches(word, 64);
        if (matches.empty()) break;
        // Complete the common prefix of every match.
        std::string common = matches.front();
        for (const auto& match : matches) {
          size_t n = 0;
          while (n < common.size() && n < match.size() &&
                 std::tolower(uint8_t(common[n])) == std::tolower(uint8_t(match[n]))) {
            ++n;
          }
          common.resize(n);
        }
        if (matches.size() == 1) common += ' ';
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, common.c_str());
        UpdateSuggestions(common);
        break;
      }
      case ImGuiInputTextFlags_CallbackEdit:
        UpdateSuggestions(std::string(data->Buf, size_t(data->BufTextLen)));
        break;
      default:
        break;
    }
    return 0;
  }

  std::vector<std::string> Matches(const std::string& prefix, size_t max_count) {
    if (Clock::now() - dvar_names_at_ > std::chrono::seconds(20) || dvar_names_.empty()) {
      dvar_names_.clear();
      for (const auto& dvar : engine::ListDvars()) dvar_names_.push_back(dvar.name);
      dvar_names_at_ = Clock::now();
    }
    std::vector<std::string> out;
    for (const auto& command : BuiltinCommands()) {
      if (StartsWithNoCase(command, prefix)) out.push_back(command);
    }
    for (const auto& name : dvar_names_) {
      if (out.size() >= max_count) break;
      if (StartsWithNoCase(name, prefix)) out.push_back(name);
    }
    return out;
  }

  // Shows the matching commands and dvars (with their values) under the command line.
  void UpdateSuggestions(const std::string& text) {
    suggestions_.clear();
    std::string word = text.substr(0, text.find(' '));
    if (word.size() < 2) return;
    bool exact_with_argument = text.find(' ') != std::string::npos;
    for (const auto& match : Matches(word, 8)) {
      if (exact_with_argument && _stricmp(match.c_str(), word.c_str()) != 0) continue;
      if (auto dvar = engine::FindDvar(match)) {
        suggestions_.push_back(dvar->name + " = \"" + dvar->value + "\"");
        if (exact_with_argument && !dvar->description.empty()) {
          suggestions_.push_back("(" + dvar->type + ", default \"" + dvar->reset + "\") " +
                                 dvar->description);
        }
      } else {
        suggestions_.push_back(match);
      }
    }
  }

  // Performance tab: frame timing, hitches and thread usage.
  void DrawPerformanceTab() {
    FrameStats frames = GetFrameStats();
    ImGui::TextColored(FpsColor(frames.fps), "%.1f FPS", frames.fps);
    ImGui::SameLine();
    ImGui::Text("  average %.2f ms   jitter %.2f ms   worst %.2f ms   1%% low %.1f FPS   "
                "frames %llu",
                frames.avg_ms, frames.jitter_ms, frames.worst_ms, frames.low_1pct_fps,
                static_cast<unsigned long long>(frames.frames));
    char label[64];
    std::snprintf(label, sizeof(label), "frame time (ms), target 16.7");
    ImGui::PlotLines("##frametime", frames.history.data(), int(frames.history.size()), 0, label,
                     0.0f, 50.0f, ImVec2(-1, 110));
    // Audio output: latency from the game to the speakers and gaps (the device found no game
    // audio queued; a rising count during play means crackles).
    ImGui::Text("Audio: output %s, latency %.0f ms, %llu gaps since startup",
                REXCVAR_GET(bo1_audio_output).c_str(), rex::audio::GetOutputLatencyMs(),
                static_cast<unsigned long long>(rex::audio::GetOutputUnderruns()));

    const float half = ImGui::GetContentRegionAvail().x * 0.5f - 4;
    ImGui::BeginChild("##hitches", ImVec2(half, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(kAccent, "Hitches (frames over 50 ms)");
    if (ImGui::BeginTable("##hitch_table", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Cause");
      ImGui::TableHeadersRow();
      for (const auto& hitch : frames.hitches) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(hitch.time.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(hitch.ms > 100 ? kBad : kFair, "%.0f", hitch.ms);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(hitch.cause.c_str());
      }
      ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##threads", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(kAccent, "CPU per thread (%% of one core)");
    auto threads = GetThreadUsage();
    if (threads.empty()) ImGui::TextDisabled("measuring...");
    for (const auto& thread : threads) {
      char overlay[32];
      std::snprintf(overlay, sizeof(overlay), "%.0f%%", thread.cpu_percent);
      ImGui::ProgressBar(float(std::min(thread.cpu_percent, 100.0) / 100.0), ImVec2(110, 0),
                         overlay);
      ImGui::SameLine();
      ImGui::TextUnformatted(thread.name.c_str());
    }
    ImGui::EndChild();
  }

  // Loading tab: what the game is reading from disk.
  void DrawLoadingTab() {
    LoadStats loads = GetLoadStats();
    ImGui::Text("Reading %.1f MB/s   %.1f MB read since startup   %zu files open",
                loads.read_rate_mbs, loads.total_read_mb, loads.open.size());
    const float half = ImGui::GetContentRegionAvail().y * 0.5f - 4;
    auto table = [](const char* id, const char* title, const char* third,
                    const std::vector<FileActivity>& files, float height) {
      ImGui::TextColored(kAccent, "%s", title);
      if (ImGui::BeginTable(id, 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingStretchProp,
                            ImVec2(0, height))) {
        ImGui::TableSetupColumn("File");
        ImGui::TableSetupColumn("MB", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(third, ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();
        for (const auto& file : files) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(file.path.c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.1f", file.megabytes);
          ImGui::TableNextColumn();
          ImGui::Text("%.2f s", file.seconds);
        }
        ImGui::EndTable();
      }
    };
    table("##open", "Open files", "Last read", loads.open, half - 20);
    table("##recent", "Recent loads", "Duration", loads.recent, 0);
  }

  void RefreshWatched() {
    if (Clock::now() - watched_at_ < std::chrono::milliseconds(250)) return;
    watched_at_ = Clock::now();
    watched_.clear();
    for (const char* name : kWatchedDvars) {
      auto dvar = engine::FindDvar(name);
      watched_.emplace_back(name, dvar ? dvar->value : std::string("-"));
    }
    toggles_.assign(std::size(kToggles), false);
    toggle_exists_.assign(std::size(kToggles), false);
    for (size_t i = 0; i < std::size(kToggles); ++i) {
      if (auto dvar = engine::FindDvar(kToggles[i].dvar)) {
        toggle_exists_[i] = true;
        // Enum dvars (e.g. cg_drawFPS) report their first value, "Off", when disabled.
        const std::string& v = dvar->value;
        toggles_[i] = !v.empty() && v != "0" && _stricmp(v.c_str(), "off") != 0 &&
                      _stricmp(v.c_str(), "false") != 0;
      }
    }
    auto map = engine::FindDvar("mapname");
    watched_map_ = map && !map->value.empty() ? map->value : "-";
  }

  // Engine tab: engine state and developer toggles.
  void DrawEngineTab() {
    RefreshWatched();
    const float half = ImGui::GetContentRegionAvail().x * 0.5f - 4;
    ImGui::BeginChild("##state", ImVec2(half, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(kAccent, "Engine state");
    if (ImGui::BeginTable("##watched", 2, ImGuiTableFlags_RowBg)) {
      for (const auto& [name, value] : watched_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kMuted, "%s", name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value.c_str());
      }
      ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextColored(kAccent, "Latest warnings and errors");
    for (const auto& [text, error] : GetNotices()) {
      ImGui::TextColored(error ? kBad : kFair, "%s", text.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##toggles", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(kAccent, "Developer toggles (engine dvars)");
    for (size_t i = 0; i < std::size(kToggles) && i < toggles_.size(); ++i) {
      if (!toggle_exists_[i]) continue;
      bool value = toggles_[i];
      std::string label = std::string(kToggles[i].label) + "##" + kToggles[i].dvar;
      if (ImGui::Checkbox(label.c_str(), &value)) {
        engine::ExecuteCommand(std::string(kToggles[i].dvar) + (value ? " 1" : " 0"));
        toggles_[i] = value;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", kToggles[i].dvar);
    }
    ImGui::Separator();
    bool dev_mode = REXCVAR_GET(bo1_dev_mode);
    if (ImGui::Checkbox("Port developer mode (bo1_dev_mode)", &dev_mode)) {
      // Only the port's diagnostics: the engine developer dvars above change how the game plays.
      rex::cvar::SetFlagByName("bo1_dev_mode", dev_mode ? "true" : "false");
    }
    ImGui::EndChild();
  }

  // Dvars tab: every engine variable, filtered.
  void DrawDvarsTab() {
    ImGui::SetNextItemWidth(260);
    bool changed = ImGui::InputTextWithHint("##dvar_filter", "Filter dvars (e.g. r_, sm_, cg_)",
                                            dvar_filter_, sizeof(dvar_filter_));
    ImGui::SameLine();
    bool refresh = ImGui::Button("Refresh");
    if (changed || refresh || dvars_.empty() ||
        Clock::now() - dvars_at_ > std::chrono::seconds(2)) {
      dvars_ = engine::ListDvars(dvar_filter_, 3000);
      dvars_at_ = Clock::now();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu dvars   (click a row to edit it in the console)", dvars_.size());
    if (ImGui::BeginTable("##dvars", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 220);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160);
      ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 120);
      ImGui::TableSetupColumn("Description");
      ImGui::TableHeadersRow();
      ImGuiListClipper clipper;
      clipper.Begin(int(dvars_.size()));
      while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          const auto& dvar = dvars_[size_t(row)];
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::Selectable(dvar.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
            std::snprintf(input_, sizeof(input_), "%s %s", dvar.name.c_str(), dvar.value.c_str());
            UpdateSuggestions(input_);
            refocus_input_ = true;
            select_tab_ = 0;
          }
          ImGui::TableNextColumn();
          bool modified = dvar.value != dvar.reset;
          ImGui::TextColored(modified ? kAccent : ImVec4(0.88f, 0.88f, 0.88f, 1.0f), "%s",
                             dvar.value.c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", dvar.reset.c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", dvar.description.c_str());
        }
      }
      ImGui::EndTable();
    }
  }

  static constexpr size_t kMaxLines = 6000;

  const char* title_;
  bool just_opened_ = true;
  int select_tab_ = -1;  // tab to bring to front on the next draw

  // Console tab.
  std::vector<LogLine> lines_;
  std::vector<size_t> visible_;
  uint64_t next_sequence_ = 0;
  char filter_[128] = {};
  int min_level_ = int(Level::kInfo);
  bool port_only_ = false;
  bool auto_scroll_ = true;
  bool scroll_to_bottom_ = false;
  char input_[512] = {};
  bool refocus_input_ = false;
  std::vector<std::string> history_;
  int history_pos_ = -1;
  std::vector<std::string> suggestions_;
  std::vector<std::string> dvar_names_;
  Clock::time_point dvar_names_at_{};

  // Engine tab.
  Clock::time_point watched_at_{};
  std::vector<std::pair<std::string, std::string>> watched_;
  std::vector<bool> toggles_, toggle_exists_;
  std::string watched_map_ = "-";

  // Dvars tab.
  char dvar_filter_[64] = {};
  std::vector<engine::DvarInfo> dvars_;
  Clock::time_point dvars_at_{};

};

DeveloperUi* g_ui = nullptr;

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateUi(rex::ui::ImGuiDrawer* drawer, const char* title) {
  auto ui = std::make_unique<DeveloperUi>(drawer, title);
  g_ui = ui.get();
  return ui;
}

void ToggleConsole() {
  bool open = !g_console_open.load();
  if (open && g_ui) g_ui->MarkOpened(-1);
  g_console_open = open;
}

void OpenConsole(const std::string& tab) {
  int index = -1;
  for (int i = 0; i < int(std::size(kTabNames)); ++i) {
    if (_stricmp(kTabNames[i], tab.c_str()) == 0) index = i;
  }
  if (g_ui) g_ui->MarkOpened(index);
  g_console_open = true;
}

void ToggleOverlay() { g_overlay_visible = !g_overlay_visible.load(); }

bool ConsoleOpen() { return g_console_open.load(); }

}  // namespace bo1::debug
