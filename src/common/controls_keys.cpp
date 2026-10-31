// Names of keys and mouse buttons: the token the settings file keeps and the name a binding shows
#include <cstdio>
#include <string>
#include <string_view>

#include "common/controls.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace controls {

namespace {

struct Name {
    Key key;
    const char* token;
    const char* display;
};

// letters, digits and F keys are made up in Find; everything else is here
constexpr Name kNames[] = {
    {0x08, "BACKSPACE", "BACKSPACE"},
    {0x09, "TAB", "TAB"},
    {0x0C, "CLEAR", "CLEAR"},
    {0x0D, "ENTER", "ENTER"},
    {0x13, "PAUSE", "PAUSE"},
    {0x14, "CAPSLOCK", "CAPS LOCK"},
    {0x1B, "ESCAPE", "ESCAPE"},
    {0x20, "SPACE", "SPACE"},
    {0x21, "PAGEUP", "PAGE UP"},
    {0x22, "PAGEDOWN", "PAGE DOWN"},
    {0x23, "END", "END"},
    {0x24, "HOME", "HOME"},
    {0x25, "LEFT", "LEFT ARROW"},
    {0x26, "UP", "UP ARROW"},
    {0x27, "RIGHT", "RIGHT ARROW"},
    {0x28, "DOWN", "DOWN ARROW"},
    {0x2C, "PRINTSCREEN", "PRINT SCREEN"},
    {0x2D, "INSERT", "INSERT"},
    {0x2E, "DELETE", "DELETE"},
    {0x5B, "LWIN", "LEFT WINDOWS"},
    {0x5C, "RWIN", "RIGHT WINDOWS"},
    {0x5D, "MENU", "MENU"},
    {0x60, "NUMPAD0", "NUM 0"},
    {0x61, "NUMPAD1", "NUM 1"},
    {0x62, "NUMPAD2", "NUM 2"},
    {0x63, "NUMPAD3", "NUM 3"},
    {0x64, "NUMPAD4", "NUM 4"},
    {0x65, "NUMPAD5", "NUM 5"},
    {0x66, "NUMPAD6", "NUM 6"},
    {0x67, "NUMPAD7", "NUM 7"},
    {0x68, "NUMPAD8", "NUM 8"},
    {0x69, "NUMPAD9", "NUM 9"},
    {0x6A, "NUMPAD_MULTIPLY", "NUM *"},
    {0x6B, "NUMPAD_PLUS", "NUM +"},
    {0x6D, "NUMPAD_MINUS", "NUM -"},
    {0x6E, "NUMPAD_PERIOD", "NUM ."},
    {0x6F, "NUMPAD_DIVIDE", "NUM /"},
    {0x90, "NUMLOCK", "NUM LOCK"},
    {0x91, "SCROLLLOCK", "SCROLL LOCK"},
    {0xA0, "LSHIFT", "LEFT SHIFT"},
    {0xA1, "RSHIFT", "RIGHT SHIFT"},
    {0xA2, "LCTRL", "LEFT CTRL"},
    {0xA3, "RCTRL", "RIGHT CTRL"},
    {0xA4, "LALT", "LEFT ALT"},
    {0xA5, "RALT", "RIGHT ALT"},
    // punctuation, named as on a US layout; Windows shows what the player's layout prints (KeyName)
    {0xBA, "SEMICOLON", ";"},
    {0xBB, "EQUALS", "="},
    {0xBC, "COMMA", ","},
    {0xBD, "MINUS", "-"},
    {0xBE, "PERIOD", "."},
    {0xBF, "SLASH", "/"},
    {0xC0, "GRAVE", "`"},
    {0xDB, "LBRACKET", "["},
    {0xDC, "BACKSLASH", "\\"},
    {0xDD, "RBRACKET", "]"},
    {0xDE, "APOSTROPHE", "'"},
    {0xDF, "OEM_8", "OEM 8"},
    {0xE2, "OEM_102", "\\"},
    {kMouseLeft, "MOUSE1", "LEFT MOUSE"},
    {kMouseRight, "MOUSE2", "RIGHT MOUSE"},
    {kMouseMiddle, "MOUSE3", "MIDDLE MOUSE"},
    {kMouse4, "MOUSE4", "MOUSE 4"},
    {kMouse5, "MOUSE5", "MOUSE 5"},
    {kWheelUp, "WHEEL_UP", "MOUSE WHEEL UP"},
    {kWheelDown, "WHEEL_DOWN", "MOUSE WHEEL DOWN"},
    {kWheelLeft, "WHEEL_LEFT", "MOUSE WHEEL LEFT"},
    {kWheelRight, "WHEEL_RIGHT", "MOUSE WHEEL RIGHT"},
};

// token and display of any code: the table, then letters, digits, F1 to F24, else VK_xx
struct Found {
    char token[16];
    char display[24];
};

Found Find(Key k) {
    Found f{};
    for (const Name& n : kNames)
        if (n.key == k) {
            std::snprintf(f.token, sizeof(f.token), "%s", n.token);
            std::snprintf(f.display, sizeof(f.display), "%s", n.display);
            return f;
        }
    if ((k >= '0' && k <= '9') || (k >= 'A' && k <= 'Z')) {
        f.token[0] = f.display[0] = char(k);
    } else if (k >= 0x70 && k <= 0x87) {
        std::snprintf(f.token, sizeof(f.token), "F%d", k - 0x70 + 1);
        std::snprintf(f.display, sizeof(f.display), "F%d", k - 0x70 + 1);
    } else if (k != kNoKey && k < 0x100) {
        std::snprintf(f.token, sizeof(f.token), "VK_%02X", unsigned(k));
        std::snprintf(f.display, sizeof(f.display), "KEY %02X", unsigned(k));
    }
    return f;
}

#ifdef _WIN32
bool Punctuation(Key k) { return (k >= 0xBA && k <= 0xC0) || (k >= 0xDB && k <= 0xDF) || k == 0xE2; }
#endif

}  // namespace

const char* KeyToken(Key k) {
    // tokens are short and few: a table of them made once, so the pointer lasts
    static const auto tokens = [] {
        static char text[kKeyCodes][16];
        for (Key i = 0; i < kKeyCodes; i++) std::snprintf(text[i], sizeof(text[i]), "%s", Find(i).token);
        return text;
    }();
    return k < kKeyCodes ? tokens[k] : "";
}

std::string KeyName(Key k) {
    if (k >= kKeyCodes) return "";
#ifdef _WIN32
    // punctuation moves between layouts (a UK keyboard has # where a US one has \): the character this one prints
    if (Punctuation(k)) {
        UINT ch = MapVirtualKeyW(k, MAPVK_VK_TO_CHAR) & 0x7FFF;
        if (ch >= 0x21 && ch < 0x7F) return std::string(1, char(ch));
    }
#endif
    return Find(k).display;
}

Key KeyFromToken(std::string_view token) {
    if (token.empty()) return kNoKey;
    for (Key i = 1; i < kKeyCodes; i++)
        if (token == KeyToken(i)) return i;
    return kNoKey;
}

}  // namespace controls
