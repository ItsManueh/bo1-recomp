// bo1 - game engine functions intercepted by the port (Com_Frame, Cbuf_AddText, Com_Error) and
// read-only access to the engine dvars

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace bo1::engine {

// Allocates the buffer in game memory used to pass commands. Call before the first frame.
void Initialize(rex::memory::Memory* memory);

// Queues a console command for the engine; it runs at the start of the next game frame.
void ExecuteCommand(const std::string& command);

// --- Dvars ---------------------------------------------------------------------------------------
// Read straight from the engine dvar hash table (no game code runs). Values are a snapshot: the
// engine may change them at any time.

struct DvarInfo {
  std::string name;
  std::string description;
  std::string type;   // "bool", "int", "float", "string", "enum", ...
  std::string value;  // current value, formatted
  std::string reset;  // default value, formatted
  std::string range;  // allowed values ("0 .. 2", "Off Simple ..."), empty if unlimited
  uint32_t flags = 0;
};

std::optional<DvarInfo> FindDvar(std::string_view name);
// Every dvar whose name contains `filter` (case-insensitive), sorted by name.
std::vector<DvarInfo> ListDvars(std::string_view filter = {}, size_t max_count = 4096);

// Number of game frames run so far (Com_Frame calls).
uint64_t FrameCount();

}  // namespace bo1::engine
