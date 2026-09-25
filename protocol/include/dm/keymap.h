// Linux evdev key codes (what Android's KeyEvent.getScanCode() reports for
// hardware keyboards) -> PC set-1 scancodes (what Windows SendInput wants).
// Codes 1..88 are identical in both sets by design; the navigation / modifier
// block needs the E0 (extended) prefix.
#pragma once

#include <cstdint>

namespace dm {

struct Scancode {
    uint16_t code = 0;  // 0 = no mapping
    bool extended = false;
};

inline Scancode evdev_to_set1(uint32_t evdev) {
    if (evdev >= 1 && evdev <= 88) return {static_cast<uint16_t>(evdev), false};
    switch (evdev) {
        case 96: return {0x1C, true};   // KP Enter
        case 97: return {0x1D, true};   // Right Ctrl
        case 98: return {0x35, true};   // KP /
        case 99: return {0x37, true};   // SysRq / Print Screen
        case 100: return {0x38, true};  // Right Alt (AltGr)
        case 102: return {0x47, true};  // Home
        case 103: return {0x48, true};  // Up
        case 104: return {0x49, true};  // Page Up
        case 105: return {0x4B, true};  // Left
        case 106: return {0x4D, true};  // Right
        case 107: return {0x4F, true};  // End
        case 108: return {0x50, true};  // Down
        case 109: return {0x51, true};  // Page Down
        case 110: return {0x52, true};  // Insert
        case 111: return {0x53, true};  // Delete
        case 113: return {0x20, true};  // Mute
        case 114: return {0x2E, true};  // Volume Down
        case 115: return {0x30, true};  // Volume Up
        case 117: return {0x59, false}; // KP =
        case 119: return {0x45, false}; // Pause (sent as NumLock scancode without E1 sequence)
        case 125: return {0x5B, true};  // Left Win
        case 126: return {0x5C, true};  // Right Win
        case 127: return {0x5D, true};  // Menu
        case 163: return {0x19, true};  // Next track
        case 164: return {0x22, true};  // Play/Pause
        case 165: return {0x10, true};  // Previous track
        case 166: return {0x24, true};  // Stop
        default: break;
    }
    if (evdev >= 183 && evdev <= 194) return {static_cast<uint16_t>(0x64 + (evdev - 183)), false};  // F13-F24
    return {};
}

}  // namespace dm
