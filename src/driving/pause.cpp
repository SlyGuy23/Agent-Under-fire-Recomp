// driving.xbe's pause menu (driving and on-rails missions): START (Escape) shuts it from any of its pages, rows or
// questions.
//
// The pause menu's input handler 0xB5B00 (thiscall: the menu, an input event) takes event 6, START, as event 1, A
// (both select through its table at 0xB6134): START picked the lit row, so on Restart or Quit Mission it asked "Are
// you sure?" and on a question it answered it. The menu keeps its page at +0x420 (0 MISSION), the lit row at +0x424
// (0 Continue Mission), a question at +0x428 (0 none, 1 restart, 2 quit) and its answer at +0x42C. Event 6 now goes in
// as a select of Continue Mission on the MISSION page with no question up: the game's own way out of the pause menu.
// Other events (B 16, D-pad down 13, right 9 ...) are the game's. Only while the pause menu is up ([0x1BEB1C] 3).
#include <cstdint>
#include <cstring>

#include "common/menus.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000B5B00);
X86_FUNC(sub_000B5B00) {
    constexpr uint32_t kRunState = 0x1BEB1C, kPaused = 3;
    constexpr uint32_t kStart = 6, kSelect = 1;
    const uint32_t menu = ctx.ecx;
    if (kernel::R32(ctx.esp + 4) == kStart && kernel::R32(kRunState) == kPaused && menu) {
        kernel::W32(menu + 0x420, 0);  // MISSION
        kernel::W32(menu + 0x424, 0);  // Continue Mission
        kernel::W32(menu + 0x428, 0);  // no question
        kernel::W32(menu + 0x42C, 0);
        kernel::W32(ctx.esp + 4, kSelect);
    }
    __imp__sub_000B5B00(ctx, base);
}

// The MISSION page's rows and marker, drawn by the menu's draw (0xBAF.., the menu in esi) with the game's own text and
// sprite routines, so the rows this port adds are drawn by them too: the same font sheet as Continue, Restart and Quit
// Mission (through the texture pack, and low-res with Menu and HUD Textures off), the same shading and shadow.
//
// A row is a text object of the menu's layout: +0x150 Continue Mission, +0x154 Restart Mission, +0x158 Quit Mission
// ("continue", "restart", "abort"). Each is drawn as 0xB20D0(object), which sets the text position [0x1C9A80] x,
// [0x1C9A84] y (the object's +0 and +4), the font [0x1C9A40] (its sheet [0x1C99F0 + font * 4]) and [0x1C9A50], then the
// row's colour [0x16B910] (lit 0xFFC4E6FF, unlit 0xFF65839F: the lit row is +0x424), then 0xB4560(object + 0x30), which
// looks its string up by id and draws it with 0xB4100(sheet, colour, x, y, string, length, 0 x 6, [0x1C9A50]) (cdecl,
// a float left on the x87 stack), first in 0x40000000 two pixels down and right when the object's +0x14 has bit 0
// (the shadow). Called from 0xBB1F1 (Continue), 0xBB23B (Quit) and 0xBB27C (Restart).
// The marker is one sprite a row (+0x1B0, +0x1B4, +0x1B8), the lit row's drawn by 0xB2BE0(sprite) from 0xBB2DE at the
// sprite's own +0 x, +4 y (made whole numbers there).
//
// The "Are you sure?" question (+0x428 non-zero while it is up, +0x42C 1 for Restart Mission, 2 for Quit Mission)
// takes the place of its row: that row is not drawn, its band (sprites +0x1F8..+0x20C for Restart, +0x210..+0x224 for
// Quit, each 0xB2510 then 0xB3BD0) and its three words ("Are you sure?", Yes, No: text objects +0xDC/+0xE4/+0xEC or
// +0xE0/+0xE8/+0xF0, 0xB4560 from 0xBB5AF.. / 0xBB695..) are drawn after the rows. The marker is not drawn meanwhile.
//
// Here: each row is drawn kLift console pixels higher (the block of five rows nearer the panel's middle), the game's
// lit row drawn unlit while one of the added rows is selected, Controls and PC Settings drawn after Quit Mission a row
// apart, and the marker beside the selected row. While a pause page is up none of them is drawn (the page is drawn
// on the panel). menus.cpp is told what was drawn (the pad's handling of the rows needs it).
namespace {

constexpr uint32_t kTextX = 0x1C9A80, kTextY = 0x1C9A84, kFont = 0x1C9A40, kFonts = 0x1C99F0, kTextArg = 0x1C9A50;
constexpr uint32_t kColour = 0x16B910, kShadowOffset = 0x143458, kInText = 0x1C9A18;
constexpr uint32_t kLit = 0xFFC4E6FFu, kUnlit = 0xFF65839Fu, kShadow = 0x40000000u;
constexpr uint32_t kDrawText = 0xB4100;
constexpr uint32_t kRetContinue = 0xBB1F6, kRetQuit = 0xBB240, kRetRestart = 0xBB281, kRetMarker = 0xBB2E3;
// the question's words and its band's sprites (the calls' return addresses)
constexpr uint32_t kRetQuestionText[] = {0xBB5B4, 0xBB5D9, 0xBB5FE, 0xBB69A, 0xBB6BE, 0xBB6E2};
constexpr uint32_t kRetQuestionBand[] = {0xBB546, 0xBB563, 0xBB586, 0xBB62C, 0xBB649, 0xBB66C};
constexpr uint32_t kRows = 0x150, kMarkers = 0x1B0, kLitRow = 0x424, kQuestion = 0x428;

float& F(uint32_t va) { return kernel::G<float>(va); }
uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// the game's rows' step and the console pixels a unit of their positions (Continue's and Restart's objects)
float RowStep(uint32_t menu) { return F(kernel::R32(menu + kRows + 4) + 4) - F(kernel::R32(menu + kRows) + 4); }
// the lift in the units of the text positions: the rows are 26 console pixels apart
float Lift(uint32_t menu) { return menus::DrivingPauseLift() * RowStep(menu) / 26.0f; }

// a string in the game's memory, written once
uint32_t GuestString(int row) {
    static uint32_t at[8] = {};
    if (row < 0 || row >= 8) return 0;
    if (!at[row]) {
        const char* text = menus::PauseExtraLabel(row);
        uint32_t n = uint32_t(std::strlen(text)) + 1;
        at[row] = mem::AllocContiguous(64, 0, 0xFFFFFFFF, 16);  // in the 64 MB the game addresses itself
        std::memcpy(kernel::HostPtr(at[row]), text, n);
    }
    return at[row];
}

void DrawText(X86Context& ctx, uint32_t text, uint32_t colour, float x, float y) {
    uint32_t length = uint32_t(std::strlen(static_cast<const char*>(kernel::HostPtr(text))));
    uint32_t sheet = kernel::R32(kFonts + kernel::R32(kFont) * 4);
    // as 0xB4560 has it around its own two calls
    kernel::W32(kInText, 1);
    kernel::CallGuest(ctx, kDrawText, {sheet, colour, Bits(x), Bits(y), text, length, 0, 0, 0, 0, 0, 0, kernel::R32(kTextArg)});
    x87_pop(ctx);  // its float result, which the game's callers drop
    kernel::W32(kInText, 0);
}

}  // namespace

bool OneOf(uint32_t ret, const uint32_t (&list)[6]) {
    for (uint32_t r : list)
        if (r == ret) return true;
    return false;
}

X86_EXTERN_FUNC(__imp__sub_000B4560);
X86_FUNC(sub_000B4560) {
    const uint32_t ret = kernel::R32(ctx.esp), menu = ctx.esi;
    int row = ret == kRetContinue ? 0 : ret == kRetRestart ? 1 : ret == kRetQuit ? 2 : -1;
    // the question's words: moved up with the rows, so they sit where the game puts them against the rows
    if (row < 0 && menu && OneOf(ret, kRetQuestionText)) {
        if (menus::PausePageUp()) {
            ctx.esp += 4;
            return;
        }
        F(kTextY) -= Lift(menu);
        __imp__sub_000B4560(ctx, base);
        return;
    }
    if (row < 0 || !menu) {
        __imp__sub_000B4560(ctx, base);
        return;
    }
    int lit = int(kernel::R32(menu + kLitRow));
    const bool question = kernel::R32(menu + kQuestion) != 0;
    menus::DrivingPauseShown(question ? -1 : lit);
    if (menus::PausePageUp()) {
        ctx.esp += 4;  // not drawn (the page is drawn on the panel instead): the call's return
        return;
    }
    int extra = menus::PauseExtraSelected();
    if (extra >= 0 && row == lit) kernel::W32(kColour, kUnlit);
    F(kTextY) -= Lift(menu);
    const uint32_t object = kernel::R32(ctx.esp + 4);
    __imp__sub_000B4560(ctx, base);
    // while a question is up the menu is the game's alone: no added rows (as the game hides nothing else, the
    // question's band and words are the only thing below its row)
    if (row != 2 || question) return;
    // Controls and PC Settings under Quit Mission, a row apart, its shadow first as the game draws it
    float x = F(kTextX), y = F(kTextY), step = RowStep(menu), off = F(kShadowOffset);
    bool shadow = kernel::G<uint8_t>(object + 0x14) & 1;
    for (int i = 0; i < menus::PauseExtraRows(); i++) {
        uint32_t text = GuestString(i);
        float yi = y + step * float(i + 1);
        if (shadow) DrawText(ctx, text, kShadow, x + off, yi + off);
        DrawText(ctx, text, extra == i ? kLit : kUnlit, x, yi);
    }
}

X86_EXTERN_FUNC(__imp__sub_000B2BE0);
X86_FUNC(sub_000B2BE0) {
    const uint32_t ret = kernel::R32(ctx.esp), menu = ctx.esi;
    if (ret != kRetMarker || !menu) {
        __imp__sub_000B2BE0(ctx, base);
        return;
    }
    menus::DrivingPauseMarker();
    if (menus::PausePageUp()) {
        ctx.esp += 4;  // not drawn: the call's return
        return;
    }
    // beside the selected row: the game's sprites are a row apart, the added rows follow on
    int extra = menus::PauseExtraSelected();
    int row = extra >= 0 ? 3 + extra : int(kernel::R32(menu + kLitRow));
    const uint32_t sprite = kernel::R32(ctx.esp + 4);
    float y0 = F(kernel::R32(menu + kMarkers) + 4), y1 = F(kernel::R32(menu + kMarkers + 4) + 4);
    float was = F(sprite + 4);
    F(sprite + 4) = y0 + (y1 - y0) * float(row) - Lift(menu);
    __imp__sub_000B2BE0(ctx, base);
    F(sprite + 4) = was;  // the menu's own layout kept
}

// the question's band: its sprites moved up with the rows (their position is the text position 0xB2510 set)
X86_EXTERN_FUNC(__imp__sub_000B3BD0);
X86_FUNC(sub_000B3BD0) {
    const uint32_t ret = kernel::R32(ctx.esp), menu = ctx.esi;
    if (!menu || !OneOf(ret, kRetQuestionBand)) {
        __imp__sub_000B3BD0(ctx, base);
        return;
    }
    if (menus::PausePageUp()) {
        ctx.esp += 4;  // not drawn: the call's return
        return;
    }
    F(kTextY) -= Lift(menu);
    __imp__sub_000B3BD0(ctx, base);
}
