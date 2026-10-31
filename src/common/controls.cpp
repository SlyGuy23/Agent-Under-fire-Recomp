// Controls of this port (see controls.h). The tables right below are the ones to edit: what each console input
// does in each context (the labels the menus show), the order the menus list them in, and the default keyboard and
// mouse layout of each context.
//
// The pad the game gets is built in three steps. The physical controller's inputs, each 0 to 1 (a stick is four
// inputs, one per direction), go through the context's remap table to the console inputs, the largest value
// winning where two land on one. With sticks::g_modern on the sticks get the round deadzone, outer deadzone and
// sensitivity first, and the result is sent as the raw value the game's own per axis deadzone turns back into it
// (see sticks.cpp, whose conversion this takes over). The keyboard and mouse build the same 0 to 1 inputs from the
// keys held, wheel pulses and, when mouse look falls back to the stick, the mouse's speed.
#include "common/controls.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

#include "common/menus.h"
#include "common/overlay.h"
#include "common/sticks.h"
#include "core/input.h"
#include "core/log.h"
#include "image_symbols.h"

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define CONTROLS_TOML 1
#else
#define CONTROLS_TOML 0  // the headless harness: no settings file
#endif

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

using A = Action;

// ---- the tables to edit ----

// What each console input does, as the menus label it, in the order of Action. On foot: the game's standard
// controller setup (its CONTROLLER option offers others, which move these around), named as its Controller Setup
// picture names them.
constexpr const char* kLabels[kContexts][kActions] = {
    // on foot (bond_eng.xbe), as the game's own Controller Setup picture names them
    {"ACTION / OPEN", "USE GADGET", "CROUCH", "JUMP", "ALT. FIRE", "CYCLE WEAPON", "PAUSE", "CENTER VIEW",
     "MANUAL AIM", "FIRE", "CROUCH (STICK CLICK)", "MANUAL AIM (STICK CLICK)",
     "NEXT WEAPON", "PREVIOUS WEAPON", "PREVIOUS GADGET", "NEXT GADGET",
     "MOVE FORWARD", "MOVE BACK", "LOOK LEFT", "LOOK RIGHT",
     "LOOK UP", "LOOK DOWN", "STRAFE LEFT", "STRAFE RIGHT", "RELOAD"},
    // driving (driving.xbe, from the missions' own control files, drivecfg.def); NOT USED: nothing in that mode
    {"FIRE PRIMARY", "HANDBRAKE", "FIRE WEAPON", "CHANGE CAMERA", "PREVIOUS WEAPON", "NEXT WEAPON", "PAUSE", "NOT USED",
     "BRAKE / REVERSE", "ACCELERATE", "NOT USED", "FIRE WEAPON (STICK CLICK)",
     "NOT USED", "NOT USED", "STEER LEFT (D-PAD)", "STEER RIGHT (D-PAD)",
     "NOT USED", "NOT USED", "STEER LEFT", "STEER RIGHT",
     "NOT USED", "LOOK BACK", "NOT USED", "NOT USED", "NOT USED"},
    // on rails (driving.xbe's gunner runs, povcfg0.def): the car drives itself
    {"RELOAD", "NOT USED", "NOT USED", "TURN TO THE OTHER SIDE", "NOT USED", "NEXT WEAPON", "PAUSE", "CENTER VIEW",
     "AIM / ZOOM", "FIRE", "NOT USED", "AIM / ZOOM (STICK CLICK)",
     "NEXT WEAPON (D-PAD)", "PREVIOUS WEAPON", "RESUME AUTO-DRIVE", "NOT USED",
     "AIM UP", "AIM DOWN", "AIM LEFT", "AIM RIGHT",
     "ZOOM IN", "ZOOM OUT", "NOT USED", "NOT USED",
     "NOT USED"},
};

// the order a menu lists the actions in; every action once
constexpr A kMenuOrder[kContexts][kActions] = {
    {A::RT, A::LT, A::Y, A::Reload, A::A, A::X, A::B, A::White, A::Black,
     A::LSUp, A::LSDown, A::RSLeft, A::RSRight, A::LSLeft, A::LSRight, A::RSUp, A::RSDown,
     A::DpadUp, A::DpadDown, A::DpadLeft, A::DpadRight, A::LSClick, A::RSClick, A::Back, A::Start},
    {A::RT, A::LT, A::LSLeft, A::LSRight, A::B, A::A, A::X, A::RSClick, A::White, A::Black, A::Y, A::RSDown,
     A::DpadLeft, A::DpadRight, A::Start,
     A::Back, A::LSClick, A::DpadUp, A::DpadDown, A::LSUp, A::LSDown, A::RSUp, A::RSLeft, A::RSRight, A::Reload},
    {A::RT, A::LT, A::RSClick, A::LSUp, A::LSDown, A::LSLeft, A::LSRight, A::RSUp, A::RSDown, A::RSLeft, A::RSRight,
     A::A, A::Y, A::White, A::DpadUp, A::DpadDown, A::DpadLeft, A::Back, A::Start,
     A::B, A::X, A::Black, A::LSClick, A::DpadRight, A::Reload},
};

// The controller's default layout, physical input to console input, where it is not the same input: on foot the
// modern one, everything that moves on the left stick (forward, back and strafing) and all the looking on the right
// (the game's own standard setup turns with the left stick and strafes with the right).
struct DefaultPad {
    A physical, gives;
};
constexpr DefaultPad kOnFootPad[] = {
    {A::LSLeft, A::RSLeft}, {A::LSRight, A::RSRight},  // strafe
    {A::RSLeft, A::LSLeft}, {A::RSRight, A::LSRight},  // turn
    {A::X, A::Reload},                                 // reload, and the gadget's use while one is out
    {A::B, A::X},                                      // crouch
};

// On rails the game aims with both sticks and zooms with the right one's Y as well: the right stick aims (as the
// game's left one, which does not zoom) and the left stick's Y zooms (as the game's right one)
constexpr DefaultPad kOnRailsPad[] = {
    {A::RSUp, A::LSUp}, {A::RSDown, A::LSDown}, {A::RSLeft, A::LSLeft}, {A::RSRight, A::LSRight},
    {A::LSUp, A::RSUp}, {A::LSDown, A::RSDown}, {A::LSLeft, A::None}, {A::LSRight, A::None},
};

// Windows virtual key codes the defaults use
constexpr Key kBackspace = 0x08, kTab = 0x09, kEnter = 0x0D, kEscape = 0x1B, kSpace = 0x20;
constexpr Key kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28, kLCtrl = 0xA2, kLShift = 0xA0;
constexpr Key kPageUp = 0x21, kPageDown = 0x22;

struct DefaultKeys {
    A action;
    Key keys[kKeySlots];
};

// On foot: a PC shooter's layout. The mouse looks around (MouseLook): no key is on turning or looking.
constexpr DefaultKeys kOnFootKeys[] = {
    {A::LSUp, {'W', kUp}},                  // move
    {A::LSDown, {'S', kDown}},
    {A::RSLeft, {'A', kLeft}},              // strafe
    {A::RSRight, {'D', kRight}},
    {A::RT, {kMouseLeft}},                  // fire (or use the gadget raised, SetFireRedirect)
    {A::LT, {kMouseRight}},                 // manual aim
    {A::Y, {kSpace}},                       // jump
    {A::A, {'E'}},                          // action, open
    {A::Reload, {'R'}},                     // reload
    {A::X, {'C', kLCtrl}},                  // crouch
    {A::B, {'G', kMouse4}},                 // use gadget
    {A::White, {'Q'}},                      // cycle weapon
    {A::Black, {'F', kMouse5}},             // alt. fire
    {A::DpadUp, {kWheelUp}},                // next weapon
    {A::DpadDown, {kWheelDown}},            // previous weapon
    {A::DpadLeft, {'Z'}},                   // previous gadget
    {A::DpadRight, {'X'}},                  // next gadget
    {A::Back, {'V', kMouseMiddle}},         // centre the view
    {A::Start, {kEscape}},                  // pause
};

// Driving: W and S on the triggers, A and D steer, the mouse buttons fire
constexpr DefaultKeys kDrivingKeys[] = {
    {A::RT, {'W', kUp}},             // accelerate
    {A::LT, {'S', kDown}},           // brake, reverse
    {A::LSLeft, {'A', kLeft}},       // steer
    {A::LSRight, {'D', kRight}},
    {A::B, {kSpace}},                // handbrake
    {A::A, {kMouseLeft}},            // fire primary
    {A::X, {kMouseRight, 'F'}},      // fire weapon
    {A::White, {kWheelUp, 'E'}},     // next weapon
    {A::Black, {kWheelDown, 'Q'}},   // previous weapon
    {A::Y, {'C'}},                   // change camera
    {A::RSDown, {kMouseMiddle, 'R'}},  // look back
    {A::Start, {kEscape}},
};

// On rails: the mouse aims (as the left stick, which only aims), left click fires
constexpr DefaultKeys kOnRailsKeys[] = {
    {A::RT, {kMouseLeft}},           // fire
    {A::LT, {kMouseRight}},          // aim, zoom
    {A::RSUp, {'W', kUp}},           // zoom in and out (the mouse aims)
    {A::RSDown, {'S', kDown}},
    {A::A, {'R'}},                   // reload
    {A::Y, {kSpace}},                // the other side
    {A::White, {kWheelUp, 'E'}},     // next weapon
    {A::DpadDown, {kWheelDown, 'Q'}},  // previous weapon
    {A::DpadLeft, {'F'}},            // resume auto-drive
    {A::Back, {'V', kMouseMiddle}},  // centre the view
    {A::Start, {kEscape}},
};

// Where mouse motion goes when mouse look falls back to a stick (no camera hook taking it), X then Y: on foot the
// game turns with the left stick's X and looks up and down with the right stick's Y
constexpr MouseAxis kMouseAxes[kContexts][2] = {
    {MouseAxis::LeftX, MouseAxis::RightY},   // on foot
    {MouseAxis::None, MouseAxis::None},      // driving: the right stick only looks back
    {MouseAxis::LeftX, MouseAxis::LeftY},    // on rails: aim (the right stick's Y zooms as well)
};

// Menus (GameState Menu and Paused): fixed keys, whatever the bindings, to the console inputs the game's menus read
struct MenuKey {
    Key key;
    A action;
    int when;  // 0 always, 1 in the front end only, 2 in the pause menu only
};
constexpr MenuKey kMenuKeys[] = {
    {kUp, A::DpadUp, 0}, {'W', A::DpadUp, 0}, {kWheelUp, A::DpadUp, 0},
    {kDown, A::DpadDown, 0}, {'S', A::DpadDown, 0}, {kWheelDown, A::DpadDown, 0},
    {kLeft, A::DpadLeft, 0}, {'A', A::DpadLeft, 0},
    {kRight, A::DpadRight, 0}, {'D', A::DpadRight, 0},
    {kEnter, A::A, 0}, {kSpace, A::A, 0}, {kMouseLeft, A::A, 0},
    {kBackspace, A::B, 0}, {kMouseRight, A::B, 0}, {kEscape, A::B, 1}, {kEscape, A::Start, 2},
    {'X', A::X, 0}, {'Y', A::Y, 0},
    {'Q', A::White, 0}, {'E', A::Black, 0}, {kPageUp, A::LT, 0}, {kPageDown, A::RT, 0}, {kTab, A::Back, 0},
};

// ---- names ----

constexpr const char* kTokens[kActions] = {
    "A", "B", "X", "Y", "BLACK", "WHITE", "START", "BACK", "LT", "RT", "LS_CLICK", "RS_CLICK",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
    "LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT", "RS_UP", "RS_DOWN", "RS_LEFT", "RS_RIGHT", "RELOAD"};

constexpr const char* kInputNames[kActions] = {
    "A", "B", "X", "Y", "BLACK", "WHITE", "START", "BACK", "LT", "RT", "LEFT STICK CLICK", "RIGHT STICK CLICK",
    "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
    "LEFT STICK UP", "LEFT STICK DOWN", "LEFT STICK LEFT", "LEFT STICK RIGHT",
    "RIGHT STICK UP", "RIGHT STICK DOWN", "RIGHT STICK LEFT", "RIGHT STICK RIGHT", "RELOAD"};

// the settings file's names (the harness has no settings file)
[[maybe_unused]] constexpr const char* kContextTokens[kContexts] = {"on_foot", "driving", "on_rails"};
[[maybe_unused]] constexpr const char* kMouseLookTokens[] = {"auto", "direct", "stick"};
[[maybe_unused]] constexpr const char* kMouseAxisTokens[] = {"none", "left_x", "left_y", "right_x", "right_y"};

[[maybe_unused]] A ActionFromToken(std::string_view t) {
    for (int i = 0; i < kActions; i++)
        if (t == kTokens[i]) return A(i);
    return A::None;
}

// ---- the image ----

constexpr bool kGame = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
constexpr bool kDriving = std::string_view(IMAGE_XBE) == "driving.xbe";
constexpr bool kThisImage = kGame || kDriving;  // the launcher has no deadzone of its own to undo
constexpr float kGameDeadzone = kGame ? 0.33f : kDriving ? 0.25f : 0.0f;  // bond_eng 0x1E1DFC, driving 0x138A88

// ---- tuning ----

constexpr float kPressed = 0.5f;          // a console button counts as held from half way
constexpr float kActive = 0.25f;          // controller input that makes it the active device, buttons and triggers
constexpr float kActiveStick = 0.25f;     // and sticks (or past the deadzone, whichever is further)
constexpr float kReleased = 0.25f;        // a held input kept from the game counts as let go under this
constexpr float kCapturePress = 0.6f;     // controller capture: pressed past this
constexpr int kMouseSwitchCounts = 8;     // mouse counts within kMouseSwitchMs that make it the active device
constexpr int64_t kMouseSwitchMs = 100;
constexpr int64_t kWheelOnMs = 50, kWheelOffMs = 50;  // a wheel notch is a press this long, then a gap
constexpr int kWheelQueue = 3;
constexpr int64_t kStickWindowMs = 32;    // mouse to stick: the speed over this much time
constexpr float kFullTiltCountsPerSecond = 1200;  // at 100%
constexpr float kDegreesPerCount = 0.044f;        // direct mouse look at 100%
constexpr int64_t kDirectConsumerMs = 500;        // Auto: Direct while TakeMouseLook was called this recently

// ---- state ----

struct ContextState {
    A remap[kActions];          // physical input to console input
    Key keys[kActions][kKeySlots];
    Options options;
};

struct Wheel {
    int pending = 0;
    bool on = false;
    int64_t start = 0;
};

struct Sample {
    int64_t t;
    int dx, dy;
};

struct CaptureRequest {
    CaptureState state = CaptureState::Idle;
    Context context = Context::OnFoot;
    Device device = Device::Controller;
    A action = A::None;
    int slot = 0;
    bool escape_cancels = true;
    A displaced = A::None;
    bool seen[4] = {};
    float prev[4][kActions] = {};
};

std::mutex g_lock;
ContextState g_state[kContexts];
int g_context_override = -1;
int64_t (*g_clock)() = nullptr;
std::atomic<Device> g_device{Device::Controller};
bool g_keyboard = false;  // a window takes keys: port 0 is always connected
std::bitset<kKeyCodes> g_held, g_suppressed;
Wheel g_wheels[4];  // up, down, left, right
std::deque<Sample> g_samples;
int64_t g_motion_start = 0;
int g_motion = 0;
double g_look_dx = 0, g_look_dy = 0;
int64_t g_last_take = -1000000;
uint32_t g_prev_active = 0;  // port 0's controller inputs past kActive at the last poll, a bit per input and stick
uint32_t g_pad_suppressed[4] = {};
CaptureRequest g_capture;
std::atomic<bool> g_blocked{false}, g_cursor_wanted{false};
GameState (*g_state_query)() = nullptr;
A (*g_fire_redirect)() = nullptr;
std::atomic<int> g_drive_mode{int(Context::Driving)};
// the toggles: the console input as the player holds it last poll, and whether the game gets it held
struct Toggle {
    bool held = false, on = false;
};
Toggle g_crouch, g_aim;
GameState g_last_state = GameState::Menu;
constexpr int kLayout = 3;  // 3: reload on its own, X with the gadget, B crouching  // settings written before 2 had the game's stick layout as the on-foot default

int64_t Now() {
    if (g_clock) return g_clock();
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Context Ctx() {
    if (g_context_override >= 0) return Context(g_context_override);
    return kDriving ? Context(g_drive_mode.load(std::memory_order_relaxed)) : Context::OnFoot;
}

GameState State() {
    if (g_context_override >= 0) return GameState::Playing;  // the unit test
    return g_state_query ? g_state_query() : GameState::Playing;
}

void DefaultBindings(Context c, Device d) {
    ContextState& s = g_state[int(c)];
    if (d == Device::Controller) {
        for (int i = 0; i < kActions; i++) s.remap[i] = A(i);
        if (c == Context::OnFoot)
            for (const DefaultPad& p : kOnFootPad) s.remap[int(p.physical)] = p.gives;
        if (c == Context::OnRails)
            for (const DefaultPad& p : kOnRailsPad) s.remap[int(p.physical)] = p.gives;
        return;
    }
    for (auto& k : s.keys) std::fill(std::begin(k), std::end(k), kNoKey);
    auto apply = [&](const auto& table) {
        for (const DefaultKeys& d : table) std::copy(std::begin(d.keys), std::end(d.keys), s.keys[int(d.action)]);
    };
    if (c == Context::OnFoot) apply(kOnFootKeys);
    else if (c == Context::Driving) apply(kDrivingKeys);
    else apply(kOnRailsKeys);
}

Options Defaults(Context c) {
    Options o;
    o.mouse_x = kMouseAxes[int(c)][0];
    o.mouse_y = kMouseAxes[int(c)][1];
    return o;
}

bool InitDefaults() {
    for (int c = 0; c < kContexts; c++) {
        DefaultBindings(Context(c), Device::Controller);
        DefaultBindings(Context(c), Device::KeyboardMouse);
        g_state[c].options = Defaults(Context(c));
    }
    return true;
}
[[maybe_unused]] const bool g_defaults = InitDefaults();

void Clamp(Options& o) {
    o.mouse_sensitivity = std::clamp<uint32_t>(o.mouse_sensitivity, 5, 1000);
    if (uint32_t(o.mouse_look) > 2) o.mouse_look = MouseLook::Auto;
    if (uint32_t(o.mouse_x) > 4) o.mouse_x = MouseAxis::None;
    if (uint32_t(o.mouse_y) > 4) o.mouse_y = MouseAxis::None;
    for (StickOptions* s : {&o.left, &o.right}) {
        s->sensitivity = std::clamp<uint32_t>(s->sensitivity, 50, 200);
        s->deadzone = std::min<uint32_t>(s->deadzone, 30);
        s->outer_deadzone = std::min<uint32_t>(s->outer_deadzone, 30);
    }
}

bool Blocked() {
    return g_blocked.load(std::memory_order_relaxed) || g_capture.state == CaptureState::Waiting || overlay::Open();
}

// A page of this port's front end menus is up (menus.cpp). Its keys come from the window messages and its
// controller input from the pad menus::Pad reads after this one, so the controller still goes through and only
// the keyboard and mouse are held back from the game.
bool PageOpen() { return menus::PageOpen(); }

// what the mouse does in this context now
MouseLook LookMode(int64_t now) {
    MouseLook m = g_state[int(Ctx())].options.mouse_look;
    if (m != MouseLook::Auto) return m;
    return now - g_last_take < kDirectConsumerMs ? MouseLook::Direct : MouseLook::Stick;
}

void Switch(Device d) {
    if (g_device.exchange(d) == d) return;
    LOGI("controls: %s", d == Device::Controller ? "controller" : "keyboard and mouse");
    if (d == Device::Controller) {
        g_look_dx = g_look_dy = 0;
        g_samples.clear();
    }
}

void KeyboardMouseUsed() { Switch(Device::KeyboardMouse); }

// ---- bindings, lock held ----

A SetKey(Context c, A a, int slot, Key k) {
    ContextState& s = g_state[int(c)];
    if (a == A::None || slot < 0 || slot >= kKeySlots || k >= kKeyCodes) return A::None;
    A displaced = A::None;
    if (k != kNoKey)
        for (int i = 0; i < kActions; i++)
            for (int j = 0; j < kKeySlots; j++)
                if (s.keys[i][j] == k && !(A(i) == a && j == slot)) {
                    s.keys[i][j] = kNoKey;
                    if (A(i) != a) displaced = A(i);
                }
    s.keys[int(a)][slot] = k;
    return displaced;
}

A Source(Context c, A a) {
    const ContextState& s = g_state[int(c)];
    if (int(a) < kPhysical && s.remap[int(a)] == a) return a;  // the usual case first: the input that gives itself
    for (int i = 0; i < kPhysical; i++)
        if (s.remap[i] == a) return A(i);
    return A::None;
}

void SetController(Context c, A a, A physical) {
    ContextState& s = g_state[int(c)];
    if (a == A::None || physical == A::None) return;
    A old = s.remap[int(physical)];
    A had = Source(c, a);
    s.remap[int(physical)] = a;
    if (had != A::None && had != physical) s.remap[int(had)] = old;
}

void FinishCapture(A displaced) {
    g_capture.state = CaptureState::Bound;
    g_capture.displaced = displaced;
}

// ---- pad building ----

float Normal(int16_t v) { return (float(v) + 0.5f) / 32767.5f; }

int16_t Raw(float f) {
    if (f == 0) return 0;
    float v = std::round(f * 32767.5f - 0.5f);
    return int16_t(std::clamp(v, -32768.0f, 32767.0f));
}

// what the game's own conversion turns into d
float Inverse(float d) {
    if (d == 0) return 0;
    float m = kGameDeadzone + std::min(std::fabs(d), 1.0f) * (1.0f - kGameDeadzone);
    return d < 0 ? -m : m;
}

struct Inputs {
    float v[kActions] = {};
};

void SetStick(Inputs& in, A up, float x, float y) {
    int u = int(up);  // up, down, left, right follow each other
    in.v[u] = std::max(y, 0.0f);
    in.v[u + 1] = std::max(-y, 0.0f);
    in.v[u + 2] = std::max(-x, 0.0f);
    in.v[u + 3] = std::max(x, 0.0f);
}

// the physical controller as it is, sticks unprocessed
Inputs Physical(const input::Pad& p) {
    Inputs in;
    constexpr A kBits[8] = {A::DpadUp, A::DpadDown, A::DpadLeft, A::DpadRight, A::Start, A::Back, A::LSClick, A::RSClick};
    for (int i = 0; i < 8; i++) in.v[int(kBits[i])] = (p.buttons >> i) & 1 ? 1.0f : 0.0f;
    constexpr A kAnalog[8] = {A::A, A::B, A::X, A::Y, A::Black, A::White, A::LT, A::RT};
    for (int i = 0; i < 8; i++) in.v[int(kAnalog[i])] = p.analog[i] / 255.0f;
    SetStick(in, A::LSUp, Normal(p.lx), Normal(p.ly));
    SetStick(in, A::RSUp, Normal(p.rx), Normal(p.ry));
    return in;
}

// the round deadzone, keeping the angle, then sensitivity; the outer deadzone is full tilt early
void ProcessStick(int16_t sx, int16_t sy, const StickOptions& o, float& dx, float& dy) {
    float x = Normal(sx), y = Normal(sy);
    float r = std::sqrt(x * x + y * y);
    float inner = o.deadzone / 100.0f, outer = o.outer_deadzone / 100.0f;
    float span = std::max(1.0f - inner - outer, 0.05f);
    dx = dy = 0;
    if (r <= inner || r <= 0) return;
    float m = std::min((r - inner) / span, 1.0f);
    m = std::min(m * o.sensitivity / 100.0f, 1.0f);
    dx = x / r * m;
    dy = y / r * m;
}

bool IsStick(int i) { return i >= int(A::LSUp); }

// a stick out of its four inputs
void StickOut(const Inputs& out, A up, float& x, float& y) {
    int u = int(up);
    y = out.v[u] - out.v[u + 1];
    x = out.v[u + 3] - out.v[u + 2];
}

// keys held together go no further than full tilt, the angle kept: W and D are half way up and right each
void RoundStick(Inputs& in, A up) {
    float x, y;
    StickOut(in, up, x, y);
    float r = std::sqrt(x * x + y * y);
    if (r > 1) SetStick(in, up, x / r, y / r);
}

input::Pad Build(const Inputs& out, bool compensate, bool connected) {
    input::Pad p;
    p.connected = connected;
    constexpr A kBits[8] = {A::DpadUp, A::DpadDown, A::DpadLeft, A::DpadRight, A::Start, A::Back, A::LSClick, A::RSClick};
    for (int i = 0; i < 8; i++)
        if (out.v[int(kBits[i])] >= kPressed) p.buttons |= uint16_t(1u << i);
    constexpr A kAnalog[8] = {A::A, A::B, A::X, A::Y, A::Black, A::White, A::LT, A::RT};
    for (int i = 0; i < 8; i++) p.analog[i] = uint8_t(std::lround(std::clamp(out.v[int(kAnalog[i])], 0.0f, 1.0f) * 255));
    // compensated, the half count bias of Normal would leave a centred axis a hair off 0 past the game's deadzone;
    // uncompensated the axis goes back exactly as it came
    auto enc = [&](float d) {
        if (!compensate) return Raw(d);
        return std::fabs(d) < 1e-4f ? int16_t(0) : Raw(Inverse(d));
    };
    float x, y;
    StickOut(out, A::LSUp, x, y);
    p.lx = enc(x), p.ly = enc(y);
    StickOut(out, A::RSUp, x, y);
    p.rx = enc(x), p.ry = enc(y);
    return p;
}

Inputs Remap(const Inputs& in, const ContextState& s) {
    Inputs out;
    for (int i = 0; i < kPhysical; i++) {
        A t = s.remap[i];
        if (t != A::None) out.v[int(t)] = std::max(out.v[int(t)], in.v[i]);
    }
    return out;
}

// the console inputs the controller gives in play: remapped, the sticks processed
Inputs ControllerInputs(int port, const input::Pad& raw, const ContextState& s) {
    bool modern = sticks::g_modern;
    Inputs in = Physical(raw);
    uint32_t& sup = g_pad_suppressed[port & 3];
    for (int i = 0; i < kActions; i++) {
        if (IsStick(i) || !(sup >> i & 1)) continue;
        if (in.v[i] < kReleased) sup &= ~(1u << i);
        else in.v[i] = 0;
    }
    if (modern) {
        float x, y;
        ProcessStick(raw.lx, raw.ly, s.options.left, x, y);
        SetStick(in, A::LSUp, x, y);
        ProcessStick(raw.rx, raw.ry, s.options.right, x, y);
        SetStick(in, A::RSUp, x, y);
    }
    return Remap(in, s);
}

// a toggled console input: each press turns it on or off
void ApplyToggle(Toggle& t, float& v) {
    bool held = v >= kPressed;
    if (held && !t.held) t.on = !t.on;
    t.held = held;
    v = t.on ? 1.0f : 0.0f;
}

// what play adds on top of the bindings: the toggles on foot, and FIRE keys using a raised gadget
void PlayExtras(Inputs& out, const Options& o, bool keyboard) {
    float reload = out.v[int(A::Reload)];
    out.v[int(A::Reload)] = 0;
    if (Ctx() != Context::OnFoot) return;
    if (reload > 0) {
        // the item in hand decides: a gadget is used (B), a weapon reloaded (A)
        A to = g_fire_redirect && g_fire_redirect() == A::B ? A::B : A::A;
        out.v[int(to)] = std::max(out.v[int(to)], reload);
    }
    if ((keyboard || o.smart_fire) && g_fire_redirect) {
        A to = g_fire_redirect();
        if (to != A::None && to != A::RT) {
            out.v[int(to)] = std::max(out.v[int(to)], out.v[int(A::RT)]);
            out.v[int(A::RT)] = 0;
        }
    }
    if (o.crouch_toggle) ApplyToggle(g_crouch, out.v[int(A::X)]);
    else g_crouch = {};
    if (o.aim_toggle) ApplyToggle(g_aim, out.v[int(A::LT)]);
    else g_aim = {};
    // Manual aim moves the crosshair with the left stick and zooms with the right stick's Y; the player has it the
    // other way round, aiming with what looks (the right stick, as the modern layout turns with it) and zooming
    // with what moves forward and back: the two Y axes trade places while aiming
    if (out.v[int(A::LT)] >= kPressed || out.v[int(A::RSClick)] >= kPressed) {
        std::swap(out.v[int(A::LSUp)], out.v[int(A::RSUp)]);
        std::swap(out.v[int(A::LSDown)], out.v[int(A::RSDown)]);
    }
}

bool WheelHeld(int w, int64_t now) {
    Wheel& h = g_wheels[w];
    if (h.on && now - h.start < kWheelOnMs) return true;
    if (h.on && now - h.start < kWheelOnMs + kWheelOffMs) return false;
    h.on = false;
    if (h.pending <= 0) return false;
    h.pending--;
    h.on = true;
    h.start = now;
    return true;
}

bool KeyHeld(Key k, int64_t now) {
    if (k == kNoKey) return false;
    if (k >= kWheelUp && k <= kWheelRight) return WheelHeld(k - kWheelUp, now);
    return g_held[k] && !g_suppressed[k];
}

// the mouse's speed as stick deflection on the axes the options name
void MouseStick(const Options& o, int64_t now, Inputs& out) {
    while (!g_samples.empty() && g_samples.front().t <= now - kStickWindowMs) g_samples.pop_front();
    int sx = 0, sy = 0;
    for (const Sample& m : g_samples) sx += m.dx, sy += m.dy;
    float k = 1000.0f / kStickWindowMs * o.mouse_sensitivity / 100.0f / kFullTiltCountsPerSecond;
    float x = sx * k, y = -sy * k * (o.mouse_invert_y ? -1 : 1);
    // on one stick the two keep their angle; on two sticks each goes to full tilt on its own
    bool one = o.mouse_x != MouseAxis::None && o.mouse_y != MouseAxis::None &&
               (int(o.mouse_x) <= 2) == (int(o.mouse_y) <= 2);
    float r = std::sqrt(x * x + y * y);
    if (one && r > 1) x /= r, y /= r;
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    auto put = [&](MouseAxis axis, float v) {
        A positive, negative;
        switch (axis) {
            case MouseAxis::LeftX: positive = A::LSRight, negative = A::LSLeft; break;
            case MouseAxis::LeftY: positive = A::LSUp, negative = A::LSDown; break;
            case MouseAxis::RightX: positive = A::RSRight, negative = A::RSLeft; break;
            case MouseAxis::RightY: positive = A::RSUp, negative = A::RSDown; break;
            default: return;
        }
        out.v[int(positive)] = std::max(out.v[int(positive)], v);
        out.v[int(negative)] = std::max(out.v[int(negative)], -v);
    };
    put(o.mouse_x, x);
    put(o.mouse_y, y);
}

Inputs KeyboardInputs(const ContextState& s, int64_t now) {
    Inputs out;
    for (int i = 0; i < kActions; i++)
        for (Key k : s.keys[i])
            if (KeyHeld(k, now)) out.v[i] = 1;
    RoundStick(out, A::LSUp);
    RoundStick(out, A::RSUp);
    if (LookMode(now) == MouseLook::Stick) MouseStick(s.options, now, out);
    return out;
}

// the menus' fixed keys (kMenuKeys)
input::Pad MenuKeyboardPad(GameState st, int64_t now) {
    Inputs out;
    for (const MenuKey& m : kMenuKeys) {
        if (m.when == 1 && st != GameState::Menu) continue;
        if (m.when == 2 && st != GameState::Paused) continue;
        if (KeyHeld(m.key, now)) out.v[int(m.action)] = 1;
    }
    return Build(out, false, true);
}

// port 0's controller became the active device when one of its inputs went past the line since the last poll
void ControllerActivity(const input::Pad& raw, const Inputs& in, const Options& o) {
    uint32_t bits = 0;
    for (int i = 0; i < int(A::LSUp); i++)
        if (in.v[i] >= kActive) bits |= 1u << i;
    auto stick = [](int16_t x, int16_t y, uint32_t dz) {
        float fx = Normal(x), fy = Normal(y);
        return std::sqrt(fx * fx + fy * fy) > std::max(kActiveStick, dz / 100.0f);
    };
    if (raw.connected && stick(raw.lx, raw.ly, o.left.deadzone)) bits |= 1u << 30;
    if (raw.connected && stick(raw.rx, raw.ry, o.right.deadzone)) bits |= 1u << 31;
    if (!raw.connected) bits = 0;
    uint32_t pressed = bits & ~g_prev_active;
    g_prev_active = bits;
    if (pressed) Switch(Device::Controller);
}

// a controller input pressed for a capture; true when it bound one
bool CaptureController(int port, const Inputs& in) {
    CaptureRequest& c = g_capture;
    int p = port & 3;
    if (!c.seen[p]) {
        c.seen[p] = true;
        std::copy(std::begin(in.v), std::end(in.v), c.prev[p]);
        return false;
    }
    int hit = -1;
    for (int i = 0; i < kPhysical && hit < 0; i++)
        if (in.v[i] >= kCapturePress && c.prev[p][i] < kCapturePress) hit = i;
    std::copy(std::begin(in.v), std::end(in.v), c.prev[p]);
    if (hit < 0) return false;
    SetController(c.context, c.action, A(hit));
    FinishCapture(A::None);
    return true;
}

// ---- keyboard and mouse events, lock held ----

void OnKey(Key k, bool down) {
    if (k == kNoKey || k >= kKeyCodes) return;
    if (!down) {
        g_held.reset(k);
        g_suppressed.reset(k);
        return;
    }
    g_held.set(k);
    constexpr Key kF10 = 0x79;
    if (g_capture.state == CaptureState::Waiting && g_capture.device == Device::KeyboardMouse && k != kF10) {
        g_suppressed.set(k);  // the key that was bound does not reach the game as well
        if (k == kEscape && g_capture.escape_cancels) g_capture.state = CaptureState::Cancelled;
        else FinishCapture(SetKey(g_capture.context, g_capture.action, g_capture.slot, k));
        return;
    }
    if (Blocked() || PageOpen()) g_suppressed.set(k);
    KeyboardMouseUsed();
}

void OnWheel(Key wheel, int notches) {
    if (wheel < kWheelUp || wheel > kWheelRight || notches <= 0) return;
    if (g_capture.state == CaptureState::Waiting && g_capture.device == Device::KeyboardMouse) {
        FinishCapture(SetKey(g_capture.context, g_capture.action, g_capture.slot, wheel));
        return;
    }
    KeyboardMouseUsed();
    if (Blocked() || PageOpen()) return;
    Wheel& w = g_wheels[wheel - kWheelUp];
    w.pending = std::min(w.pending + notches, kWheelQueue);
}

void OnMotion(int dx, int dy) {
    int64_t now = Now();
    if (now - g_motion_start > kMouseSwitchMs) {
        g_motion_start = now;
        g_motion = 0;
    }
    g_motion += std::abs(dx) + std::abs(dy);
    if (g_motion >= kMouseSwitchCounts) KeyboardMouseUsed();
    if (g_device != Device::KeyboardMouse || Blocked() || PageOpen()) return;
    if (LookMode(now) == MouseLook::Direct) {
        g_look_dx += dx;
        g_look_dy += dy;
    } else {
        g_samples.push_back({now, dx, dy});
        while (g_samples.size() > 1024 || (!g_samples.empty() && g_samples.front().t < now - 1000)) g_samples.pop_front();
    }
}

void OnFocusLost() {
    g_held.reset();
    g_suppressed.reset();
    for (Wheel& w : g_wheels) w = {};
    g_samples.clear();
    g_look_dx = g_look_dy = 0;
}

}  // namespace

// ---- names and bindings ----

Context CurrentContext() {
    std::lock_guard lock(g_lock);
    return Ctx();
}

Device ActiveDevice() { return g_device.load(); }

void SetGameStateQuery(GameState (*query)()) {
    std::lock_guard lock(g_lock);
    g_state_query = query;
}

GameState CurrentGameState() {
    std::lock_guard lock(g_lock);
    return State();
}

void SetDrivingMode(Context c) {
    if (c == Context::Driving || c == Context::OnRails) g_drive_mode.store(int(c));
}

void SetFireRedirect(Action (*query)()) {
    std::lock_guard lock(g_lock);
    g_fire_redirect = query;
}

const char* ActionToken(Action a) { return int(a) < kActions ? kTokens[int(a)] : "NONE"; }
const char* ActionInputName(Action a) { return int(a) < kActions ? kInputNames[int(a)] : ""; }
const char* ActionLabel(Context c, Action a) { return int(a) < kActions ? kLabels[std::min(int(c), kContexts - 1)][int(a)] : ""; }

std::vector<Action> Actions(Context c, Device) {
    const A* order = kMenuOrder[std::min(int(c), kContexts - 1)];
    return std::vector<Action>(order, order + kActions);
}

Key KeyBinding(Context c, Action a, int slot) {
    std::lock_guard lock(g_lock);
    if (int(a) >= kActions || slot < 0 || slot >= kKeySlots) return kNoKey;
    return g_state[int(c)].keys[int(a)][slot];
}

Action SetKeyBinding(Context c, Action a, int slot, Key k) {
    std::lock_guard lock(g_lock);
    return SetKey(c, a, slot, k);
}

Action ControllerSource(Context c, Action a) {
    std::lock_guard lock(g_lock);
    return int(a) < kActions ? Source(c, a) : A::None;
}

Action ControllerTarget(Context c, Action physical) {
    std::lock_guard lock(g_lock);
    return int(physical) < kActions ? g_state[int(c)].remap[int(physical)] : A::None;
}

void SetControllerBinding(Context c, Action a, Action physical) {
    std::lock_guard lock(g_lock);
    if (int(a) < kActions && int(physical) < kPhysical) SetController(c, a, physical);
}

void ClearControllerInput(Context c, Action physical) {
    std::lock_guard lock(g_lock);
    if (int(physical) < kPhysical) g_state[int(c)].remap[int(physical)] = A::None;
}

std::string BindingName(Context c, Device d, Action a, int slot) {
    if (d == Device::KeyboardMouse) return KeyName(KeyBinding(c, a, slot));
    A s = ControllerSource(c, a);
    // on foot with nothing on USE GADGET, reload's input uses a gadget that is out
    if (s == A::None && a == A::B && c == Context::OnFoot) s = ControllerSource(c, A::Reload);
    return s == A::None ? "" : kInputNames[int(s)];
}

void ResetBindings(Context c, Device d) {
    std::lock_guard lock(g_lock);
    DefaultBindings(c, d);
}

Options GetOptions(Context c) {
    std::lock_guard lock(g_lock);
    return g_state[int(c)].options;
}

void SetOptions(Context c, const Options& o) {
    std::lock_guard lock(g_lock);
    Options& s = g_state[int(c)].options;
    s = o;
    Clamp(s);
}

Options DefaultOptions(Context c) { return Defaults(c); }

void ResetOptions(Context c) {
    std::lock_guard lock(g_lock);
    g_state[int(c)].options = Defaults(c);
}

// ---- capture ----

void BeginCapture(Context c, Device d, Action a, int slot, bool escape_cancels) {
    std::lock_guard lock(g_lock);
    g_capture = CaptureRequest{};
    if (int(a) >= kActions) return;
    g_capture.state = CaptureState::Waiting;
    g_capture.context = c;
    g_capture.device = d;
    g_capture.action = a;
    g_capture.slot = std::clamp(slot, 0, kKeySlots - 1);
    g_capture.escape_cancels = escape_cancels;
}

void CancelCapture() {
    std::lock_guard lock(g_lock);
    if (g_capture.state == CaptureState::Waiting) g_capture.state = CaptureState::Cancelled;
}

CaptureState Capture(Action* displaced) {
    std::lock_guard lock(g_lock);
    if (displaced) *displaced = g_capture.displaced;
    return g_capture.state;
}

void EndCapture() {
    std::lock_guard lock(g_lock);
    g_capture = CaptureRequest{};
}

// ---- game input and mouse look ----

void SetGameInputBlocked(bool blocked) { g_blocked.store(blocked, std::memory_order_relaxed); }
void SetCursorWanted(bool wanted) { g_cursor_wanted.store(wanted, std::memory_order_relaxed); }

bool TakeMouseLook(float& yaw_degrees, float& pitch_degrees) {
    std::lock_guard lock(g_lock);
    int64_t now = Now();
    const Options& o = g_state[int(Ctx())].options;
    yaw_degrees = pitch_degrees = 0;
    if (o.mouse_look == MouseLook::Stick) return false;
    bool was_direct = LookMode(now) == MouseLook::Direct;
    g_last_take = now;
    double dx = g_look_dx, dy = g_look_dy;
    g_look_dx = g_look_dy = 0;
    if (!was_direct || g_device != Device::KeyboardMouse || Blocked() || PageOpen()) return false;
    float k = kDegreesPerCount * o.mouse_sensitivity / 100.0f;
    yaw_degrees = float(dx) * k;
    pitch_degrees = float(-dy) * k * (o.mouse_invert_y ? -1 : 1);
    return true;
}

void Pad(int port, input::Pad& pad) {
    std::lock_guard lock(g_lock);
    int64_t now = Now();
    const ContextState& s = g_state[int(Ctx())];
    Inputs phys = Physical(pad);
    if (!pad.connected) phys = Inputs{};
    if (port == 0) ControllerActivity(pad, phys, s.options);
    bool bound = g_capture.state == CaptureState::Waiting && g_capture.device == Device::Controller && pad.connected &&
                 CaptureController(port, phys);
    bool connected = pad.connected || (port == 0 && g_keyboard);
    if (Blocked() || bound) {
        // what is held now stays away from the game until let go
        uint32_t& sup = g_pad_suppressed[port & 3];
        for (int i = 0; i < int(A::LSUp); i++)
            if (phys.v[i] >= kReleased) sup |= 1u << i;
        if (port == 0) {
            g_suppressed |= g_held;
            for (Wheel& w : g_wheels) w = {};
            g_samples.clear();
        }
        pad = input::Pad{};
        pad.connected = connected;
        return;
    }
    if (port == 0 && PageOpen()) {
        g_suppressed |= g_held;
        for (Wheel& w : g_wheels) w = {};
        g_samples.clear();
    }
    GameState st = State();
    if (port == 0 && st != g_last_state) {
        // from a menu into play or back: what is held now stays away from the game until let go (Escape closing
        // the pause menu must not open it again)
        g_last_state = st;
        g_suppressed |= g_held;
        for (Wheel& w : g_wheels) w = {};
        uint32_t& sup = g_pad_suppressed[0];
        for (int i = 0; i < int(A::LSUp); i++)
            if (phys.v[i] >= kReleased) sup |= 1u << i;
    }
    if (st != GameState::Playing) {
        // the menus: the controller as it is, the menu keys
        if (st == GameState::Menu) g_crouch = g_aim = {};
        if (port == 0 && g_keyboard && g_device == Device::KeyboardMouse) {
            pad = MenuKeyboardPad(st, now);
            return;
        }
        // buttons kept from the game stay kept until let go
        uint32_t& sup = g_pad_suppressed[port & 3];
        constexpr A kBits[8] = {A::DpadUp, A::DpadDown, A::DpadLeft, A::DpadRight, A::Start, A::Back, A::LSClick, A::RSClick};
        constexpr A kAnalog[8] = {A::A, A::B, A::X, A::Y, A::Black, A::White, A::LT, A::RT};
        for (int i = 0; i < 8; i++) {
            int b = int(kBits[i]), an = int(kAnalog[i]);
            if (sup >> b & 1) {
                if (phys.v[b] < kReleased) sup &= ~(1u << b);
                else pad.buttons &= uint16_t(~(1u << i));
            }
            if (sup >> an & 1) {
                if (phys.v[an] < kReleased) sup &= ~(1u << an);
                else pad.analog[i] = 0;
            }
        }
        pad.connected = connected;
        return;
    }
    if (port == 0 && g_keyboard && g_device == Device::KeyboardMouse) {
        Inputs out = KeyboardInputs(s, now);
        PlayExtras(out, s.options, true);
        pad = Build(out, kThisImage, true);
        return;
    }
    if (!pad.connected) {
        pad.connected = connected;
        return;
    }
    Inputs out = ControllerInputs(port, pad, s);
    if (port == 0) PlayExtras(out, s.options, false);
    pad = Build(out, sticks::g_modern && kThisImage, pad.connected);
}

// ---- the settings file ----

void Load(const void* table) {
#if CONTROLS_TOML
    const toml::table& t = *static_cast<const toml::table*>(table);
    std::lock_guard lock(g_lock);
    const toml::table* all = t["controls"].as_table();
    int layout = all ? int((*all)["layout"].value_or(int64_t(1))) : kLayout;
    if (!all) {
        // a settings file from before these controls: its one stick deadzone goes to every stick
        for (ContextState& s : g_state) s.options.left.deadzone = s.options.right.deadzone = std::min<uint32_t>(sticks::g_deadzone, 30);
        return;
    }
    for (int c = 0; c < kContexts; c++) {
        const toml::table* x = (*all)[kContextTokens[c]].as_table();
        if (!x) continue;
        ContextState& s = g_state[c];
        Options& o = s.options;
        o.mouse_sensitivity = (*x)["mouse_sensitivity"].value_or(o.mouse_sensitivity);
        o.mouse_invert_y = (*x)["mouse_invert_y"].value_or(o.mouse_invert_y);
        std::string look = (*x)["mouse_look"].value_or(std::string(kMouseLookTokens[int(o.mouse_look)]));
        for (int i = 0; i < 3; i++)
            if (look == kMouseLookTokens[i]) o.mouse_look = MouseLook(i);
        auto axis = [&](const char* key, MouseAxis& a) {
            std::string v = (*x)[key].value_or(std::string(kMouseAxisTokens[int(a)]));
            for (int i = 0; i < 5; i++)
                if (v == kMouseAxisTokens[i]) a = MouseAxis(i);
        };
        axis("mouse_x", o.mouse_x);
        axis("mouse_y", o.mouse_y);
        auto stick = [&](const char* name, StickOptions& so) {
            std::string n = name;
            so.sensitivity = (*x)[n + "_sensitivity"].value_or(so.sensitivity);
            so.deadzone = (*x)[n + "_deadzone"].value_or(so.deadzone);
            so.outer_deadzone = (*x)[n + "_outer_deadzone"].value_or(so.outer_deadzone);
        };
        stick("left_stick", o.left);
        stick("right_stick", o.right);
        o.crouch_toggle = (*x)["crouch_toggle"].value_or(o.crouch_toggle);
        o.aim_toggle = (*x)["aim_toggle"].value_or(o.aim_toggle);
        o.smart_fire = (*x)["smart_fire"].value_or(o.smart_fire);
        Clamp(o);
        // keys: an action left out keeps its default, an empty list is unbound, "" an empty slot
        if (const toml::table* kb = (*x)["keyboard"].as_table()) {
            for (auto&& [name, node] : *kb) {
                A a = ActionFromToken(name.str());
                if (a == A::None) continue;
                Key keys[kKeySlots] = {};
                int n = 0;
                if (const toml::array* arr = node.as_array()) {
                    for (const toml::node& e : *arr)
                        if (auto str = e.value<std::string>(); str && n < kKeySlots) keys[n++] = KeyFromToken(*str);
                } else if (auto str = node.value<std::string>()) {
                    keys[0] = KeyFromToken(*str);
                }
                std::copy(std::begin(keys), std::end(keys), s.keys[int(a)]);
            }
        }
        // controller: physical input = console input it gives, or NONE
        // the modern stick layout came in with layout 2: older on-foot controller bindings give way to it
        const toml::table* pad = (*x)["controller"].as_table();
        if (layout < 3 && (c == int(Context::OnFoot) || c == int(Context::OnRails))) pad = nullptr;
        if (pad) {
            for (auto&& [name, node] : *pad) {
                A physical = ActionFromToken(name.str());
                auto target = node.value<std::string>();
                if (physical == A::None || !target) continue;
                s.remap[int(physical)] = ActionFromToken(*target);  // NONE and unknown names: nothing
            }
        }
    }
#else
    (void)table;
#endif
}

void Save(void* table) {
#if CONTROLS_TOML
    toml::table& t = *static_cast<toml::table*>(table);
    std::lock_guard lock(g_lock);
    toml::table all;
    for (int c = 0; c < kContexts; c++) {
        const ContextState& s = g_state[c];
        const Options& o = s.options;
        toml::table x{{"mouse_sensitivity", int64_t(o.mouse_sensitivity)},
                      {"mouse_invert_y", o.mouse_invert_y},
                      {"mouse_look", kMouseLookTokens[int(o.mouse_look)]},
                      {"mouse_x", kMouseAxisTokens[int(o.mouse_x)]},
                      {"mouse_y", kMouseAxisTokens[int(o.mouse_y)]},
                      {"left_stick_sensitivity", int64_t(o.left.sensitivity)},
                      {"left_stick_deadzone", int64_t(o.left.deadzone)},
                      {"left_stick_outer_deadzone", int64_t(o.left.outer_deadzone)},
                      {"right_stick_sensitivity", int64_t(o.right.sensitivity)},
                      {"right_stick_deadzone", int64_t(o.right.deadzone)},
                      {"right_stick_outer_deadzone", int64_t(o.right.outer_deadzone)},
                      {"crouch_toggle", o.crouch_toggle},
                      {"aim_toggle", o.aim_toggle},
                      {"smart_fire", o.smart_fire}};
        toml::table kb, pad;
        for (int i = 0; i < kActions; i++) {
            // each slot in its place, "" for an empty one before a bound one
            int last = kKeySlots - 1;
            while (last >= 0 && s.keys[i][last] == kNoKey) last--;
            toml::array keys;
            for (int j = 0; j <= last; j++) keys.push_back(KeyToken(s.keys[i][j]));
            kb.insert(kTokens[i], std::move(keys));
            pad.insert(kTokens[i], ActionToken(s.remap[i]));
        }
        x.insert("keyboard", std::move(kb));
        x.insert("controller", std::move(pad));
        all.insert(kContextTokens[c], std::move(x));
    }
    all.insert("layout", int64_t(kLayout));
    t.insert_or_assign("controls", std::move(all));
#else
    (void)table;
#endif
}

// ---- tests ----

namespace testing {

void SetContext(Context c) {
    std::lock_guard lock(g_lock);
    g_context_override = int(c);
}

void SetClock(int64_t (*now_ms)()) {
    std::lock_guard lock(g_lock);
    g_clock = now_ms;
}

void KeyEvent(Key k, bool down) {
    std::lock_guard lock(g_lock);
    OnKey(k, down);
}

void WheelEvent(Key wheel, int notches) {
    std::lock_guard lock(g_lock);
    OnWheel(wheel, notches);
}

void MouseMotion(int dx, int dy) {
    std::lock_guard lock(g_lock);
    OnMotion(dx, dy);
}

void FocusLost() {
    std::lock_guard lock(g_lock);
    OnFocusLost();
}

void EnableKeyboard() {
    std::lock_guard lock(g_lock);
    g_keyboard = true;
}

void ResetAll() {
    std::lock_guard lock(g_lock);
    InitDefaults();
    OnFocusLost();
    g_device = Device::Controller;
    g_keyboard = true;
    g_motion = 0;
    g_motion_start = 0;
    g_last_take = -1000000;
    g_prev_active = 0;
    std::fill(std::begin(g_pad_suppressed), std::end(g_pad_suppressed), 0u);
    g_capture = CaptureRequest{};
    g_crouch = g_aim = {};
    g_last_state = GameState::Playing;
    g_blocked = false;
    g_cursor_wanted = false;
}

}  // namespace testing

}  // namespace controls

// ---- the window: keys, raw mouse input and the cursor ----

#ifndef _WIN32
namespace controls {
bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }
void Window(void*) {}
}  // namespace controls
#else
namespace controls {

namespace {

constexpr UINT_PTR kCursorTimer = 0xC047;  // the cursor is checked on this timer, 20 times a second
HWND g_hwnd = nullptr;
bool g_hidden = false;  // window thread only
bool g_have_absolute = false;
LONG g_abs_x = 0, g_abs_y = 0;
int g_wheel_rest[2] = {};  // parts of a notch from smooth wheels, vertical and horizontal

// left and right Shift, Ctrl and Alt apart
Key Translate(WPARAM wp, LPARAM lp) {
    UINT vk = UINT(wp);
    UINT scan = (UINT(lp) >> 16) & 0xFF;
    bool extended = (lp >> 24) & 1;
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = extended ? VK_RMENU : VK_LMENU;
    return vk < 0x100 ? Key(vk) : kNoKey;
}

void RawMouse(LPARAM lp) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    int dx = 0, dy = 0;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // remote desktops and tablets: positions 0 to 65535 over the screen, so the change from the last one
        bool virt = m.usFlags & MOUSE_VIRTUAL_DESKTOP;
        int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        LONG x = LONG(m.lLastX / 65535.0 * w), y = LONG(m.lLastY / 65535.0 * h);
        if (g_have_absolute) dx = x - g_abs_x, dy = y - g_abs_y;
        g_abs_x = x, g_abs_y = y;
        g_have_absolute = true;
    } else {
        dx = m.lLastX;
        dy = m.lLastY;
    }
    USHORT f = m.usButtonFlags;
    std::lock_guard lock(g_lock);
    if (dx || dy) OnMotion(dx, dy);
    constexpr std::pair<USHORT, Key> kButtons[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, kMouseLeft}, {RI_MOUSE_RIGHT_BUTTON_DOWN, kMouseRight},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, kMouseMiddle}, {RI_MOUSE_BUTTON_4_DOWN, kMouse4}, {RI_MOUSE_BUTTON_5_DOWN, kMouse5}};
    for (auto [down, key] : kButtons) {
        if (f & down) OnKey(key, true);
        if (f & (down << 1)) OnKey(key, false);  // each button's up flag is the bit above its down flag
    }
    auto wheel = [&](int axis, Key positive, Key negative) {
        int& rest = g_wheel_rest[axis];
        rest += SHORT(m.usButtonData);
        int notches = rest / WHEEL_DELTA;
        rest -= notches * WHEEL_DELTA;
        if (notches > 0) OnWheel(positive, notches);
        if (notches < 0) OnWheel(negative, -notches);
    };
    if (f & RI_MOUSE_WHEEL) wheel(0, kWheelUp, kWheelDown);
    if (f & RI_MOUSE_HWHEEL) wheel(1, kWheelRight, kWheelLeft);
}

// Hidden in the middle of the window while the keyboard and mouse play, the game's window focused and nothing
// wanting the cursor. Hidden, and left free, while the controller is in use (moving the mouse brings it back).
// Shown and let go otherwise.
bool g_clipped = false;
void UpdateCursor(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd && !IsIconic(hwnd);
    bool capture = g_device == Device::KeyboardMouse && !g_cursor_wanted.load(std::memory_order_relaxed) &&
                   !overlay::Open() && !PageOpen() && focused;
    bool hide = capture || (g_device == Device::Controller && focused);
    if (capture) {
        RECT r;
        GetClientRect(hwnd, &r);
        POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(hwnd, &c);
        RECT clip{c.x - 1, c.y - 1, c.x + 1, c.y + 1};
        ClipCursor(&clip);  // again each time: Windows lets it go on its own when the focus moves
        g_clipped = true;
    } else if (g_clipped) {
        g_clipped = false;
        ClipCursor(nullptr);
    }
    if (hide && !g_hidden) {
        g_hidden = true;
        SetCursor(nullptr);
    } else if (!hide && g_hidden) {
        g_hidden = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

}  // namespace

void Window(void* hwnd) {
    HWND h = static_cast<HWND>(hwnd);
    if (!h || g_hwnd == h) return;
    g_hwnd = h;
    // raw mouse motion, with the usual mouse messages still sent for the menus
    RAWINPUTDEVICE d{0x01, 0x02, 0, h};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&d, 1, sizeof(d))) LOGE("controls: raw mouse input not registered (%lu)", GetLastError());
    SetTimer(h, kCursorTimer, 50, nullptr);
    std::lock_guard lock(g_lock);
    g_keyboard = true;
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    HWND h = static_cast<HWND>(hwnd);
    if (!g_hwnd) Window(h);
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            Key k = Translate(wp, lp);
            bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            bool capturing;
            {
                std::lock_guard lock(g_lock);
                capturing = down && k != VK_F10 && g_capture.state == CaptureState::Waiting &&
                            g_capture.device == Device::KeyboardMouse;
                if (!down) OnKey(k, false);
                else if (!((lp >> 30) & 1)) OnKey(k, true);  // not the key repeating
            }
            // the key being bound (or Escape cancelling) is not also a step in the menu showing the capture
            if (capturing) {
                *result = 0;
                return true;
            }
            // Alt on its own would open the window menu, which holds the game until a key or click lets it go
            if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && (k == VK_LMENU || k == VK_RMENU) && g_clipped) {
                *result = 0;
                return true;
            }
            return false;
        }
        case WM_INPUT:
            RawMouse(LPARAM(lp));
            return false;  // DefWindowProc frees the input
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                {
                    std::lock_guard lock(g_lock);
                    OnFocusLost();
                }
                UpdateCursor(h);
            }
            return false;
        case WM_KILLFOCUS: {
            std::lock_guard lock(g_lock);
            OnFocusLost();
            return false;
        }
        case WM_TIMER:
            if (wp != kCursorTimer) return false;
            UpdateCursor(h);
            *result = 0;
            return true;
        case WM_SETCURSOR:
            if (!g_hidden || LOWORD(lp) != HTCLIENT) return false;
            SetCursor(nullptr);
            *result = TRUE;
            return true;
        default:
            return false;
    }
}

}  // namespace controls
#endif
