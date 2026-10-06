// bo1 - local players: split screen profiles and input device assignment

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, bo1_split_screen);
REXCVAR_DECLARE(std::string, bo1_player_names);
REXCVAR_DECLARE(std::string, bo1_keyboard_player);
REXCVAR_DECLARE(std::string, bo1_split_screen_view);
REXCVAR_DECLARE(bool, bo1_split_screen_boost);

namespace rex::input {
class InputSystem;
}

namespace bo1::players {

// Maps the port options onto the runtime options (sign-in of players 2-4, device assignment).
// Call before the runtime is set up: the input system reads them when it is created.
void ApplyRuntimeOptions();

// Engine commands (dvars) for the split screen view; applied after every map change.
std::vector<std::string> EngineCommands();

// Called a few times per second with the number of views the engine draws (r_num_viewports):
// while split screen is on, the emulated GPU command thread gets the highest priority.
void OnViewCount(int views);

// Debug/test input: holds a controller button of a player (0-3) for hold_ms milliseconds, on top of
// whatever the real controller does. Buttons: a b x y start back up down left right lb rb ls rs lt
// rt. Returns false for an unknown button or player.
bool Press(uint32_t user_index, const std::string& button, int hold_ms);

// Buttons (XINPUT bits) and triggers injected right now for a player; false when none are held.
// sequence counts press starts and ends (0 = never injected): it is added to the packet number so
// the game sees every change.
bool InjectedInput(uint32_t user_index, uint16_t& buttons, uint8_t& left_trigger,
                   uint8_t& right_trigger, uint32_t& sequence);

// Tests: connects bo1_test_pads virtual controllers (idle, driven only by Press), so split screen
// can be tested on a PC without spare controllers. Call before the game polls its controllers.
void AddTestPads(rex::input::InputSystem* input);

}  // namespace bo1::players
