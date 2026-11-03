// Controls of this port: controller remapping, stick response, keyboard and mouse, mouse look and the rebinding
// the menus offer. The game only ever reads a console controller, so everything here ends as the input::Pad
// GameInfo::pad hands it.
//
// Two contexts, each with its own bindings and options: on foot (bond_eng.xbe, which also runs the front end, and
// default.xbe, which only plays the intro films) and driving (driving.xbe). Every executable loads and saves both,
// so the on-foot menus can edit the driving bindings.
//
// An action is one console controller input: a button, a trigger, a D-pad direction or one direction of a stick.
// The controller table says, per physical input on the player's pad, which console input it gives (identity by
// default, a swap on rebinding so nothing is lost). The keyboard and mouse table gives each console input up to
// two keys or mouse buttons. Whichever device was used last drives the game; the other is ignored until it is used.
//
// Threads: Message and Window run on the window thread, Pad on whichever thread polls the controller (guest
// threads and the port poller), the rest from anywhere (the menus run on the render thread). One lock covers it all.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace input {
struct Pad;
}

namespace controls {

// on foot (bond_eng.xbe), driving and the on-rails gunner runs (both driving.xbe, told apart by the level)
enum class Context : uint8_t { OnFoot, Driving, OnRails };
inline constexpr int kContexts = 3;

// where the player is: Menu (front end, loading), Paused (the in-mission pause menu) or Playing. Remapping, the
// keyboard and mouse layout and the toggles apply only while Playing; the menus get the controller as it is and a
// fixed set of menu keys (arrows or WASD, Enter, Escape...), so they always work the same.
enum class GameState : uint8_t { Menu, Paused, Playing };
void SetGameStateQuery(GameState (*query)());
// on foot: true while a cutscene plays, when Enter, Space and the left mouse button skip it (send A)
void SetCutsceneQuery(bool (*query)());
GameState CurrentGameState();

enum class Device : uint8_t { Controller, KeyboardMouse };

// the console controller's inputs; also names the physical inputs of the player's controller in the remap table
enum class Action : uint8_t {
    A, B, X, Y, Black, White, Start, Back, LT, RT, LSClick, RSClick,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LSUp, LSDown, LSLeft, LSRight,
    RSUp, RSDown, RSLeft, RSRight,
    // not a console input: on foot, A (reload) with a weapon in hand and B (use it) with a gadget; never a physical
    // input of the remap table
    Reload,
    None,
};
inline constexpr int kActions = int(Action::None);
inline constexpr int kPhysical = int(Action::Reload);  // the inputs a controller has
inline constexpr int kKeySlots = 2;  // keyboard and mouse bindings per action

// keys are Windows virtual key codes (left and right Shift, Ctrl and Alt apart: 0xA0 to 0xA5), mouse buttons and
// wheel directions above them
using Key = uint16_t;
inline constexpr Key kNoKey = 0;
inline constexpr Key kMouseLeft = 0x101, kMouseRight = 0x102, kMouseMiddle = 0x103, kMouse4 = 0x104, kMouse5 = 0x105;
inline constexpr Key kWheelUp = 0x110, kWheelDown = 0x111, kWheelLeft = 0x112, kWheelRight = 0x113;
inline constexpr Key kKeyCodes = 0x120;  // every Key is below this

// how mouse motion looks around: Direct hands it to the camera hook (TakeMouseLook), Stick turns its speed into
// stick deflection on the axes Options::mouse_x and mouse_y name, Auto is Direct while something takes it
// (TakeMouseLook called in the last half second) and Stick otherwise
enum class MouseLook : uint8_t { Auto, Direct, Stick };
// the stick axis mouse motion moves when it falls back to a stick: the game turns with the left stick's X in its
// standard controller setup and strafes with the right one's
enum class MouseAxis : uint8_t { None, LeftX, LeftY, RightX, RightY };

struct StickOptions {
    uint32_t sensitivity = 100;   // percent of the game's turn rate (or steering) at each tilt, 50 to 200
    uint32_t deadzone = 10;       // percent of full tilt, round, 0 to 30
    uint32_t outer_deadzone = 0;  // percent short of full tilt that already counts as full tilt, 0 to 30
};

struct Options {
    uint32_t mouse_sensitivity = 100;  // percent, 5 to 1000
    bool mouse_invert_y = false;
    MouseLook mouse_look = MouseLook::Auto;
    MouseAxis mouse_x = MouseAxis::RightX, mouse_y = MouseAxis::RightY;  // Stick only; each context's defaults differ
    StickOptions left, right;  // the controller's sticks; sticks::g_modern off leaves them as they are
    bool crouch_toggle = false;  // on foot: a press crouches, the next stands (else held)
    bool aim_toggle = false;     // on foot: a press brings up the manual aim, the next puts it away
    bool kbm_auto_aim = false;   // on foot and on rails, the keyboard and mouse: the game's auto-aim (lock-on)
    bool smart_fire = true;      // on foot, the controller: FIRE with a gadget raised uses it (the keyboard always does)
};

// driving.xbe: whether this run is a driving mission or an on-rails one
void SetDrivingMode(Context c);
// On foot each query is asked for the controller port whose pad is being built (0 for the keyboard and mouse); in
// multiplayer split screen game_info.cpp answers for the local player on that port.
// on foot with the keyboard and mouse: what the keys bound to FIRE give instead (the gadget's use while one is
// raised); Action::RT, or Action::None, keeps them on FIRE
void SetFireRedirect(Action (*query)(int port));
// on foot: Reload pressed with a weapon in hand (the game's own reload request, game_info.cpp)
void SetReloadRequest(void (*request)(int port));
// on foot: true while the port's player is on a mounted gun (game_info.cpp), which the game aims with its left stick
void SetMountedGunQuery(bool (*query)(int port));
// on foot, the device in use has Reload bound: A (action) no longer reloads (src/game/hooks.cpp)
bool SeparateReload();
// on foot: the local players (a bit each) with a reload asked for. The game has one request for everyone
// ([0x333524]); src/game/hooks.cpp hands it to each player's own think, so in split screen it reloads the right gun.
uint32_t ReloadRequests();
void SetReloadRequested(int player, bool requested);

// where this executable plays, and which device drives it now
Context CurrentContext();
Device ActiveDevice();
// the game's auto-aim is off now: the keyboard and mouse in use, its auto lock-on option off (hooks.cpp)
bool AimAssistOff();

// names: the settings file token ("RT", "LS_UP"), the console input as a binding shows it ("RT", "LEFT STICK UP"),
// and what the action does in that context ("FIRE WEAPON", from the label table at the top of controls.cpp)
const char* ActionToken(Action a);
const char* ActionInputName(Action a);
const char* ActionLabel(Context c, Action a);
// the actions a menu lists for that context and device, in menu order
std::vector<Action> Actions(Context c, Device d);

// keys: the token kept in the settings file ("W", "LCTRL", "MOUSE1", "WHEEL_UP"), the name shown ("W", "LEFT CTRL",
// "LEFT MOUSE", "MOUSE WHEEL UP"; on Windows punctuation keys show what the keyboard layout prints)
const char* KeyToken(Key k);
std::string KeyName(Key k);
Key KeyFromToken(std::string_view token);  // kNoKey when unknown

// bindings. A key goes to one action of a context: binding it elsewhere takes it away there (that action is
// returned, Action::None when it had none). A controller input binds by swapping: the physical input that gave
// the action before now gives what the newly bound one gave.
Key KeyBinding(Context c, Action a, int slot);
Action SetKeyBinding(Context c, Action a, int slot, Key k);  // kNoKey clears the slot
Action ControllerSource(Context c, Action a);  // the physical input that gives a, Action::None when none does
Action ControllerTarget(Context c, Action physical);
void SetControllerBinding(Context c, Action a, Action physical);
void ClearControllerInput(Context c, Action physical);  // that physical input gives nothing
// the binding as the menu shows it: a key name or controller input name, "" when unbound (slot is for keys)
std::string BindingName(Context c, Device d, Action a, int slot = 0);
void ResetBindings(Context c, Device d);

Options GetOptions(Context c);
void SetOptions(Context c, const Options& o);  // clamped to the ranges above
void ResetOptions(Context c);
Options DefaultOptions(Context c);
// changes are kept in memory; the caller saves the settings file (app::Save) when it wants them kept

// capture of the next physical input for a binding. Keyboard and mouse: the next key, mouse button or wheel turn
// (F10 is left to the settings panel; Escape cancels when escape_cancels). Controller: the next button, trigger or
// stick direction pressed past half way on any controller. While waiting the game gets a neutral pad.
enum class CaptureState : uint8_t { Idle, Waiting, Bound, Cancelled };
void BeginCapture(Context c, Device d, Action a, int slot = 0, bool escape_cancels = true);
void CancelCapture();
// Bound and Cancelled stay until EndCapture or the next BeginCapture; displaced is the action that lost the key
CaptureState Capture(Action* displaced = nullptr);
void EndCapture();

// the game gets a neutral pad while blocked, and the inputs held when it ends stay away from the game until
// released. The F10 panel and a capture block it on their own. A page of menus.cpp (menus::PageOpen) holds back
// only the keyboard and mouse, the same way: its keys come as window messages, and the controller pad still goes on
// to menus::Pad, which reads it for the page and keeps it from the game itself.
void SetGameInputBlocked(bool blocked);
// something wants the mouse: the cursor is shown and let go. The F10 panel, a menus.cpp page and an unfocused
// window do the same on their own.
void SetCursorWanted(bool wanted);
// On a menu (the front end, a pause menu) the cursor is let go, and the Windows arrow shown once the mouse moves
// there while the keyboard and mouse are in use; true while it is shown. It goes again in play (held in the middle of
// the window) and when the controller is used.
bool MenuCursor();

// mouse motion since the last take, as degrees: yaw right and pitch up positive, sensitivity and invert Y applied
// (0.044 degrees a count at 100%). False with zeros while the keyboard and mouse are not driving the game, the
// game's input is blocked or this context's mouse look is Stick. Call once a game frame from the camera hook.
bool TakeMouseLook(float& yaw_degrees, float& pitch_degrees);

// How much faster than the game's own the turning from each console stick axis of a port's last pad is to be, for
// the hooks that scale it (src/game/hooks.cpp on foot, src/driving/hooks.cpp on rails): the stick sensitivity (or
// the mouse as a stick past full tilt) where the game cannot be sent it as a stick value. On foot the part past
// full tilt (under 100% the stick itself is sent less far); on rails all of it, the game having only three rates
// (and the further of the two sticks' on each axis, which the game takes). 1 everywhere else and outside play.
void TurnGains(float& lx, float& ly, float& rx, float& ry);  // port 0
void TurnGains(int port, float& lx, float& ly, float& rx, float& ry);

// GameInfo hooks
void Pad(int port, input::Pad& pad);  // from GameInfo::pad: in place of sticks::Apply, before menus::Pad
// GameInfo::message, before the panel's and the menus': sees every key; takes only a key down while a capture
// waits for one, Alt on its own while the cursor is held, its timer, and WM_SETCURSOR while the cursor is hidden
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Window(void* hwnd);  // GameInfo::window: raw mouse input and the cursor timer
void Load(const void* table);  // GameInfo::load, the settings file's toml::table
void Save(void* table);        // GameInfo::save

// for the unit test: the input the window messages would give, a clock and the context
namespace testing {
void SetContext(Context c);
void SetClock(int64_t (*now_ms)());  // nullptr: the steady clock
void KeyEvent(Key k, bool down);     // a key or mouse button
void WheelEvent(Key wheel, int notches);
void MouseMotion(int dx, int dy);
void FocusLost();
void ResetAll();  // defaults everywhere, nothing held, controller active
void EnableKeyboard();  // the headless harness: a keyboard as a window would give
}  // namespace testing

}  // namespace controls
