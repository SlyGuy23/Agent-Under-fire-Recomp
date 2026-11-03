#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot; the game code window starts it earlier
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port (the one GameInfo::pad_device names), then GameInfo::pad
Pad Poll(int port);
// the controller plugged into that slot (0 to 3, in the order they were plugged in) as it is, GameInfo::pad left out
Pad Device(int slot);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
