#include "common/menus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/controls.h"
#include "common/menu_font.h"
#include "common/overlay.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/shaders.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace menus {

namespace {

constexpr bool kMenus = std::string_view(IMAGE_XBE) == "bond_eng.xbe";
constexpr bool kDrivingImage = std::string_view(IMAGE_XBE) == "driving.xbe";

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// a screen counts as up while its rows were drawn in this frame or the one before (frames counted by the game's swaps)
uint64_t Frame() { return app::g_swaps.load(std::memory_order_relaxed); }
// seen holds the frame number plus one, 0 never
// seen this frame or one of the last few: when the game draws a frame less often than it presents (loading, quitting
// a mission) what the pages add would otherwise blink on and off with it
bool Recent(uint64_t seen) { return seen && Frame() + 1 >= seen && Frame() + 1 - seen <= 3; }

// ---------------------------------------------------------------------------------------------------------------
// the draws: bond_eng.xbe's menus are lists of float4 positions under an identity projection, two triangles a quad

constexpr uint32_t kTriangleList = 5, kFloat4 = 0x42, kFloat2 = 0x22;

struct Layout {
    uint32_t pos = 0, color = 0, tc = 0;
    bool ok = false;
};

// a 2D menu draw: no program, the identity projection, float4 positions in stream 0 with a float4 colour and
// coordinates beside them
Layout MenuLayout(const gpu::State& s, uint32_t prim, uint32_t stride, uint32_t count) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Layout l;
    if (prim != kTriangleList || count < 6 || count % 6 || !s.vs || !s.vs->program.empty()) return l;
    if (std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return l;
    const auto& pos = s.vs->inputs[0];
    const auto& tc = s.vs->inputs[9];
    if (pos.stream != 0 || pos.type != kFloat4 || tc.stream != 0 || (tc.type != kFloat4 && tc.type != kFloat2)) return l;
    int color = -1;
    for (int r = 1; r < 16; r++) {
        const auto& in = s.vs->inputs[size_t(r)];
        if (r != 9 && in.stream == 0 && in.type == kFloat4) color = int(in.offset);
    }
    if (color < 0 || pos.offset + 16 > stride || tc.offset + 8 > stride || uint32_t(color) + 16 > stride) return l;
    l.pos = pos.offset;
    l.tc = tc.offset;
    l.color = uint32_t(color);
    l.ok = true;
    return l;
}

struct Box {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
};

float* At(uint8_t* v, uint32_t stride, uint32_t i, uint32_t off) { return reinterpret_cast<float*>(v + i * stride + off); }
const float* At(const uint8_t* v, uint32_t stride, uint32_t i, uint32_t off) {
    return reinterpret_cast<const float*>(v + i * stride + off);
}

Box BoxOf(const uint8_t* v, uint32_t stride, const Layout& l, uint32_t first, uint32_t n) {
    Box b;
    for (uint32_t i = first; i < first + n; i++) {
        const float* p = At(v, stride, i, l.pos);
        const float* t = At(v, stride, i, l.tc);
        b.x0 = std::min(b.x0, p[0]), b.x1 = std::max(b.x1, p[0]), b.y0 = std::min(b.y0, p[1]), b.y1 = std::max(b.y1, p[1]);
        b.u0 = std::min(b.u0, t[0]), b.u1 = std::max(b.u1, t[0]), b.v0 = std::min(b.v0, t[1]), b.v1 = std::max(b.v1, t[1]);
    }
    return b;
}

bool Near(float a, float b, float e = 1.5f) { return std::fabs(a - b) <= e; }

void MoveY(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, float dy) {
    for (uint32_t i = 0; i < count; i++) At(v, stride, i, l.pos)[1] += dy;
}

void SetColor(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, const std::array<float, 4>& c) {
    for (uint32_t i = 0; i < count; i++) std::memcpy(At(v, stride, i, l.color), c.data(), 16);
}

std::array<float, 4> ColorOf(const uint8_t* v, uint32_t stride, const Layout& l) {
    std::array<float, 4> c;
    std::memcpy(c.data(), At(v, stride, 0, l.color), 16);
    return c;
}

// collapsed to its first vertex: nothing drawn
void Hide(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count) {
    float x = At(v, stride, 0, l.pos)[0], y = At(v, stride, 0, l.pos)[1];
    for (uint32_t i = 0; i < count; i++) {
        At(v, stride, i, l.pos)[0] = x;
        At(v, stride, i, l.pos)[1] = y;
    }
}

enum class Page { None, Quit, PcSettings, Controls, PauseControls, PausePcSettings };
std::atomic<Page> g_page{Page::None};
// one of the pause menu's pages (Controls, PC Settings) is up
bool PausePage() {
    Page p = g_page.load();
    return p == Page::PauseControls || p == Page::PausePcSettings;
}
// START or Escape on a pause page: the page stays up (and the pause menu's rows hidden) until the game has shut its
// pause menu, so the menu is never seen for a frame on the way back to the game. When asked, 0 none.
std::atomic<int64_t> g_closing_at{0};
bool Closing() { return g_closing_at.load() != 0; }

bool TextureSize(const gpu::State& s, uint32_t w, uint32_t h) {
    gpu::TextureDesc d;
    return s.textures[0] && gpu::Describe(s.textures[0], d) && d.width == w && d.height == h;
}

// ---------------------------------------------------------------------------------------------------------------
// the main menu: five bars from a 256x256 sheet of 20 row bands, 0 to 4 the entries lit, 5 to 9 the same unlit.
// The texture pack's image of the sheet adds band 10 (QUIT GAME lit) and 11 (unlit) in the sheet's empty rows.

constexpr float kBand = 20.0f / 256.0f;
constexpr int kMainItems = 5, kQuitLit = 10, kQuitUnlit = 11;
constexpr float kBarX0 = 128, kBarX1 = 485;
// the game's bar tops, and this port's: six rows a little closer, clear of the footer at 407
constexpr float kMainTops[kMainItems] = {131, 184, 238, 291, 345};
constexpr float kNewTop = 117, kNewStep = 49;

std::atomic<uint64_t> g_main_seen{0};
std::atomic<int> g_main_selected{-1};  // the game's lit bar
std::atomic<bool> g_quit_selected{false};
std::atomic<bool> g_escape_close{false};  // ESCAPE on the pause menu's Controls page: the pause menu shuts
std::atomic<int64_t> g_close_pause_at{0};  // when START or ESCAPE asked bond_eng.xbe's pause menu to shut, 0 none
std::atomic<int> g_quit_game_row{-1};  // the game's selection under QUIT GAME: Options, or Start Mission when come to by wrapping

// band of a bar quad, -1 when not one
int BarBand(const gpu::State& s, const Box& b) {
    if (!Near(b.x0, kBarX0, 3) || !Near(b.x1, kBarX1, 3) || b.u0 > 0.01f || b.u1 < 0.98f) return -1;
    float band = b.v0 / kBand;
    int k = int(std::lround(band));
    if (std::fabs(band - k) > 0.02f || std::fabs((b.v1 - b.v0) - kBand) > 0.002f || k < 0 || k >= 2 * kMainItems) return -1;
    return TextureSize(s, 256, 256) ? k : -1;
}

void SetBand(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, int from, int to) {
    for (uint32_t i = 0; i < count; i++) At(v, stride, i, l.tc)[1] += float(to - from) * kBand;
}

// ---------------------------------------------------------------------------------------------------------------
// the options list: one line of text a draw, six vertices a letter, a black shadow drawn a pixel down and right first

constexpr int kOptionRows = 10;
constexpr float kRowTop = 115, kRowStep = 26.8f;
constexpr float kLabelX = 96, kValueX = 352;
constexpr const char* kLabels[kOptionRows] = {"Difficulty",  "Controller Setup", "Invert Pitch", "Vibration",
                                              "HUD Always Visible", "Cross Hair", "Gold Rewards", "Music",
                                              "Sound Effects", "Credits"};
constexpr int kControllerRow = 1;    // PC Settings goes under it
constexpr float kInsert = 27;        // rows below Controller Setup move down this far
constexpr const char* kPcSettings = "PC Settings";

struct Glyph {
    bool known = false;
    float u0, v0, u1, v1;  // texture rectangle
    float w, h, dy;        // size, and top below the line's first capital's
    float advance;
};
std::mutex g_glyph_lock;
std::array<Glyph, 128> g_glyphs;
// The list's letters are learnt per font sheet (its pack key): the options list is drawn from more than one sheet in
// the course of the game, and one sheet's letters laid out with another's coordinates came out as other letters.
struct GlyphSet {
    std::array<Glyph, 128> glyphs{};
    std::array<std::array<bool, 2>, 6> corners{};
    bool corners_known = false;
};
std::unordered_map<uint64_t, GlyphSet> g_glyph_sets;
uint64_t g_glyph_key = 0;
// for each of a letter's six vertices: right, bottom
std::array<std::array<bool, 2>, 6> g_corners{};
bool g_corners_known = false;

// with g_glyph_lock held: the letters of the sheet with this key in g_glyphs
void UseGlyphs(uint64_t key) {
    if (key == g_glyph_key) return;
    GlyphSet& cur = g_glyph_sets[g_glyph_key];
    cur.glyphs = g_glyphs, cur.corners = g_corners, cur.corners_known = g_corners_known;
    GlyphSet& next = g_glyph_sets[key];
    g_glyphs = next.glyphs, g_corners = next.corners, g_corners_known = next.corners_known;
    g_glyph_key = key;
}
uint64_t StageKey(const gpu::State& s);

std::atomic<uint64_t> g_options_seen{0};
std::atomic<uint64_t> g_options_title_seen{0};  // the frame the OPTIONS title (7 italic letters at 90,78) was drawn in
std::atomic<int> g_options_selected{-1};
std::atomic<bool> g_pc_selected{false};
std::atomic<int> g_pc_game_row{-1};  // the game's selection under PC Settings: Controller Setup or Invert Pitch
std::array<float, 4> g_lit_color{0.58f, 0.91f, 1.0f, 1.0f};
std::array<float, 4> g_unlit_color{0.36f, 0.405f, 0.46f, 1.0f};
std::array<float, 4> g_unlit_value_color{0.36f, 0.405f, 0.46f, 1.0f};

// a text line of the options list: its row, -1 when not one; value column or label column
int OptionRow(const gpu::State& s, const Box& b, bool& value) {
    if (!TextureSize(s, 256, 128)) return -1;
    float x = b.x0;
    if (Near(x, kLabelX, 4)) value = false;
    else if (Near(x, kValueX, 6)) value = true;
    else return -1;
    float r = (b.y0 - kRowTop) / kRowStep;
    int row = int(std::lround(r));
    if (row < 0 || row >= kOptionRows || std::fabs(r - row) > 0.25f) return -1;
    return row;
}

bool IsShadow(const std::array<float, 4>& c) { return c[0] < 0.02f && c[1] < 0.02f && c[2] < 0.02f; }
float Brightness(const std::array<float, 4>& c) { return c[0] + c[1] + c[2]; }

// the letters of a known label, learnt from the game's own line
void Learn(uint64_t key, const uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, std::string_view text) {
    if (count != text.size() * 6) return;
    std::lock_guard lock(g_glyph_lock);
    UseGlyphs(key);
    Box first = BoxOf(v, stride, l, 0, 6);
    if (!g_corners_known) {
        for (uint32_t i = 0; i < 6; i++) {
            const float* p = At(v, stride, i, l.pos);
            g_corners[i] = {std::fabs(p[0] - first.x1) < std::fabs(p[0] - first.x0),
                            std::fabs(p[1] - first.y1) < std::fabs(p[1] - first.y0)};
        }
        g_corners_known = true;
    }
    for (size_t k = 0; k < text.size(); k++) {
        unsigned char c = static_cast<unsigned char>(text[k]);
        if (c >= 128 || g_glyphs[c].known) continue;
        Box b = BoxOf(v, stride, l, uint32_t(k * 6), 6);
        Glyph& g = g_glyphs[c];
        g.u0 = b.u0, g.v0 = b.v0, g.u1 = b.u1, g.v1 = b.v1;
        g.w = b.x1 - b.x0, g.h = b.y1 - b.y0, g.dy = b.y0 - first.y0;
        if (k + 1 < text.size()) {
            Box n = BoxOf(v, stride, l, uint32_t((k + 1) * 6), 6);
            g.advance = n.x0 - b.x0;
        } else {
            continue;  // its advance is not known yet
        }
        g.known = true;
    }
}

// Letters no options label has, at their place in the list font's 256x128 sheet: left and right column and the top
// of their row of cells, given in the row of a letter that is learnt ('e', whose row of cells starts at 80) so the
// cell's height and offsets carry over.
struct SheetCell {
    char c;
    int x0, x1, row_top, below;  // below: rows the letter reaches under the reference letter's cell
};
constexpr SheetCell kSheetCells[] = {{'g', 119, 127, 66, 2}};
constexpr char kRefLetter = 'e';
constexpr int kRefRowTop = 80;

void FillFromSheet() {
    const Glyph& ref = g_glyphs[static_cast<unsigned char>(kRefLetter)];
    if (!ref.known) return;
    float px_per_texel = ref.w / ((ref.u1 - ref.u0) * 256.0f);
    for (const SheetCell& cell : kSheetCells) {
        Glyph& g = g_glyphs[static_cast<unsigned char>(cell.c)];
        if (g.known) continue;
        g.u0 = float(cell.x0) / 256.0f;
        g.u1 = float(cell.x1) / 256.0f;
        g.v0 = ref.v0 + float(cell.row_top - kRefRowTop) / 128.0f;
        g.v1 = ref.v1 + float(cell.row_top - kRefRowTop + cell.below) / 128.0f;
        g.w = float(cell.x1 - cell.x0) * px_per_texel;
        g.h = ref.h + float(cell.below) * px_per_texel;
        g.dy = ref.dy;
        g.advance = g.w + ref.advance - ref.w;
        g.known = true;
    }
}

// text in the list's lettering, laid out from a line of the game's (template: its first letter's six vertices)
std::vector<uint8_t> Text(uint64_t key, std::string_view text, const uint8_t* tmpl, uint32_t stride, const Layout& l, float x, float y,
                          const std::array<float, 4>& color) {
    std::lock_guard lock(g_glyph_lock);
    UseGlyphs(key);
    std::vector<uint8_t> out;
    if (!g_corners_known) return out;
    FillFromSheet();
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 128 || !g_glyphs[c].known) return {};
    }
    out.resize(text.size() * 6 * stride);
    for (size_t k = 0; k < text.size(); k++) {
        const Glyph& g = g_glyphs[static_cast<unsigned char>(text[k])];
        for (uint32_t i = 0; i < 6; i++) {
            uint8_t* d = out.data() + (k * 6 + i) * stride;
            std::memcpy(d, tmpl + i * stride, stride);
            float* p = reinterpret_cast<float*>(d + l.pos);
            float* t = reinterpret_cast<float*>(d + l.tc);
            bool right = g_corners[i][0], bottom = g_corners[i][1];
            p[0] = x + (right ? g.w : 0);
            p[1] = y + g.dy + (bottom ? g.h : 0);
            t[0] = right ? g.u1 : g.u0;
            t[1] = bottom ? g.v1 : g.v0;
            std::memcpy(d + l.color, color.data(), 16);
        }
        x += g.advance;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// the pause menu's MISSION page: Continue Mission, Restart Mission and Quit Mission in the italic lettering, rows 26
// apart from y 173 at x 111 (a black shadow a pixel down and right first), the >> marker (a 64x64 sheet) 3 above the
// selected row. Controls and PC Settings go under Quit Mission.

constexpr float kPauseX = 111, kPauseTop = 173, kPauseStep = 26, kMarkerX = 87, kMarkerAbove = 3;
constexpr int kPauseRows = 3;
// the rows added under the game's, in order (the pause menus of bond_eng.xbe and driving.xbe alike)
constexpr const char* kPauseExtra[] = {"Controls", "PC Settings"};
constexpr int kPauseExtraRows = int(sizeof(kPauseExtra) / sizeof(kPauseExtra[0]));
std::atomic<uint64_t> g_pause_seen{0};
std::atomic<int> g_pause_lit{-1};
// the added row selected (an index of kPauseExtra), -1 while the game's own selection holds
std::atomic<int> g_pause_extra{-1};
std::array<float, 4> g_pause_lit_color{0.83f, 0.93f, 1.0f, 1.0f}, g_pause_unlit_color{0.45f, 0.54f, 0.62f, 1.0f};

bool Paused() { return controls::CurrentGameState() == controls::GameState::Paused; }

// The five rows sat low on the panel, PC Settings against the Lives Remaining / Mission Duration bar: the game's rows,
// the added ones and the marker all go this far up, the block in the middle of the panel.
constexpr float kPauseLift = 20;

// a row of the pause menu's MISSION page, -1 when not one; *lifted: already moved up by kPauseLift (a line the game
// keeps and draws again is moved once only). The two grids are 6 apart, each row is matched to 3.
int PauseRow(const gpu::State& s, const Box& b, bool* lifted = nullptr) {
    if (!TextureSize(s, 256, 128) || !Near(b.x0, kPauseX, 3)) return -1;
    for (int up = 0; up < 2; up++) {
        float r = (b.y0 - (kPauseTop - (up ? kPauseLift : 0))) / kPauseStep;
        int row = int(std::lround(r));
        if (row < 0 || row >= kPauseRows || std::fabs(r - row) * kPauseStep > 3) continue;
        if (lifted) *lifted = up != 0;
        return row;
    }
    return -1;
}

// where the >> marker goes (its top): beside the added row selected, else beside the game's row it is at, moved up with
// the rows. y0: where it is drawn, on the game's grid or already moved; elsewhere (on its way between rows) it only
// moves up.
float PauseMarkerTarget(float y0, int extra) {
    for (int up = 0; up < 2; up++) {
        float top = kPauseTop - kMarkerAbove - (up ? kPauseLift : 0);
        float r = (y0 - top) / kPauseStep;
        int row = int(std::lround(r));
        if (std::fabs(r - row) * kPauseStep > 3) continue;
        if (extra >= 0) row = kPauseRows + extra;
        return kPauseTop - kMarkerAbove - kPauseLift + kPauseStep * float(row);
    }
    return y0 - kPauseLift;
}

// text in the italic lettering (menu_font.h) laid out from a line of the game's in it: its first letter's vertices
// give the vertex order and everything else that is not position, coordinates or colour
std::vector<uint8_t> ItalicText(std::string_view text, const uint8_t* tmpl, uint32_t stride, const Layout& l, float x,
                                float y, const std::array<float, 4>& color) {
    std::vector<uint8_t> out;
    Box first = BoxOf(tmpl, stride, l, 0, 6);
    std::array<std::array<bool, 2>, 6> corners;
    for (uint32_t i = 0; i < 6; i++) {
        const float* p = At(tmpl, stride, i, l.pos);
        corners[i] = {std::fabs(p[0] - first.x1) < std::fabs(p[0] - first.x0), std::fabs(p[1] - first.y1) < std::fabs(p[1] - first.y0)};
    }
    for (size_t k = 0; k < text.size(); k++) {
        char ch = text[k];
        const MenuGlyph* g = FindGlyph(kTitleFont, ch);
        if (!g) {
            x += kTitleFont.space;
            continue;
        }
        size_t at = out.size();
        out.resize(at + 6 * stride);
        for (uint32_t i = 0; i < 6; i++) {
            uint8_t* d = out.data() + at + i * stride;
            std::memcpy(d, tmpl + i * stride, stride);
            float* p = reinterpret_cast<float*>(d + l.pos);
            float* t = reinterpret_cast<float*>(d + l.tc);
            bool right = corners[i][0], bottom = corners[i][1];
            p[0] = x + (right ? float(g->x1 - g->x0 + 1) : 0);
            p[1] = y + float(g->dy) + (bottom ? float(g->y1 - g->y0 + 1) : 0);
            t[0] = float(right ? g->x1 : g->x0) / 256.0f;
            t[1] = float(bottom ? g->y1 : g->y0) / 128.0f;
            std::memcpy(d + l.color, color.data(), 16);
        }
        // even gaps between the letters (menu_font.h): the table's advances leave "Settin gs"
        x += k + 1 < text.size() ? float(ItalicAdvance(ch, text[k + 1])) : float(g->advance);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// the Controller Setup shown before a mission: its title, CONTROLLER SETUP (16 letters, 96 vertices) in the italic
// lettering at 90,78, then the controller's picture and its two pictures of labels (256x256 sheets). Those go, and
// a list of the mission's controls takes their place.

std::atomic<uint64_t> g_setup_seen{0};
uint64_t g_setup_frame = ~0ull;  // the frame whose title was drawn: its pictures that follow go
controls::Context (*g_mission_query)() = nullptr;

// the options screen's title (OPTIONS, italic, at 90,78) while a page is up: it reads as the page; the game's line
// is kept here (its vertices, before they are collapsed) for AfterDraw to lay the page's name out from
std::vector<uint8_t> g_title_line;
Box g_title_box;
bool OptionsTitle(const gpu::State& s, const Box& b) {
    Page p = g_page.load();
    return (p == Page::Controls || p == Page::PcSettings) && Recent(g_options_seen.load()) && TextureSize(s, 256, 128) &&
           Near(b.x0, 90.5f, 2) && Near(b.y0, 78.5f, 2);
}

bool SetupTitle(const gpu::State& s, const Box& b, uint32_t count) {
    // the letters' box starts at 90,76 and the shadow's a pixel down and right (less once tucked in): either counts
    return count == 96 && TextureSize(s, 256, 128) && b.x0 > 87 && b.x0 < 94 && b.y0 > 73 && b.y0 < 81;
}

// ---------------------------------------------------------------------------------------------------------------
// A pause page (Controls, PC Settings) is drawn on the pause menu's own panel: its moving background, frame, bars and
// prompts stay, its words, rows, marker and running figure go, and the panel is drawn taller (each piece's height
// stretched between the panel's top and bottom, the small pictures and letters on it moved with it rather than
// stretched), so the page's rows fit inside it. Console pixels, 640x480.

struct PanelStretch {
    float s0, s1;  // the panel's top and bottom as the game draws it
    float d0, d1;  // and while a page is up
};
constexpr PanelStretch kFootPanel{97, 353, 55, 425}, kDrivePanel{108, 338, 62, 418};
// the words that go start above this (tabs, rows, Lives Remaining and Mission Duration, the tops of their letters):
// the prompts' bar under them stays
constexpr float kFootWordsEnd = 310, kDriveWordsEnd = 306;

float StretchY(const PanelStretch& p, float y) { return p.d0 + (y - p.s0) * (p.d1 - p.d0) / (p.s1 - p.s0); }
bool InPanel(const PanelStretch& p, float x0, float y0, float y1) { return x0 > 10 && y0 >= p.s0 - 2 && y1 <= p.s1 + 2; }
// a piece moved with the panel but kept its size (a letter, a prompt's picture), else stretched
bool KeepsSize(float w, float h) { return w <= 48 && h <= 24; }

// bond_eng.xbe while a pause page is up: a 2D draw (two triangles a quad, float4 positions under the identity
// projection, textured or not) of the panel. Its words, the >> marker and the running figure go (true: nothing to
// draw), the rest is stretched (StretchPanel).
void StretchPanel(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count);
bool PausePanelDraw(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (prim != kTriangleList || count < 6 || count % 6 || !s.vs || !s.vs->program.empty()) return false;
    if (std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return false;
    const auto& pos = s.vs->inputs[0];
    if (pos.stream != 0 || pos.type != kFloat4 || pos.offset + 16 > stride) return false;
    Layout l;
    l.pos = pos.offset;
    Box b = BoxOf(v, stride, l, 0, count);
    // the running figure (a 128x128 picture right of the rows)
    if (count == 6 && TextureSize(s, 128, 128) && b.x0 > 450 && b.y0 > 150 && b.y0 < kFootPanel.s1) {
        Hide(v, stride, l, count);
        return true;
    }
    if (!InPanel(kFootPanel, b.x0, b.y0, b.y1)) return false;
    bool glyphs = true;  // a line of letters: every quad letter-sized
    for (uint32_t q = 0; glyphs && q < count; q += 6) {
        Box g = BoxOf(v, stride, l, q, 6);
        glyphs = g.x1 - g.x0 < 48 && g.y1 - g.y0 < 48;
    }
    // the menu is still up underneath (its Quit Mission row drawn): the pad's handling of it holds
    if (PauseRow(s, b) == kPauseRows - 1) g_pause_seen.store(Frame() + 1);
    bool marker = count == 6 && TextureSize(s, 64, 64) && Near(b.x0, kMarkerX, 3);
    bool words = glyphs && s.textures[0] && TextureSize(s, 256, 128) && b.y0 < kFootWordsEnd;
    if (marker || words) {
        Hide(v, stride, l, count);
        return true;
    }
    StretchPanel(v, stride, l, count);
    return false;
}

// bond_eng.xbe: a draw of the panel, two triangles a quad. A line of letters the game keeps and draws again from the
// same memory is stretched once: the first vertex's height written last time is remembered with the memory's place.
void StretchPanel(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count) {
    static std::mutex lock;
    static std::unordered_map<const uint8_t*, float> done;
    std::lock_guard g(lock);
    if (auto it = done.find(v); it != done.end() && At(v, stride, 0, l.pos)[1] == it->second) return;
    for (uint32_t q = 0; q + 6 <= count; q += 6) {
        Box b = BoxOf(v, stride, l, q, 6);
        if (KeepsSize(b.x1 - b.x0, b.y1 - b.y0)) {
            float dy = StretchY(kFootPanel, (b.y0 + b.y1) / 2) - (b.y0 + b.y1) / 2;
            for (uint32_t i = q; i < q + 6; i++) At(v, stride, i, l.pos)[1] += dy;
        } else {
            for (uint32_t i = q; i < q + 6; i++) At(v, stride, i, l.pos)[1] = StretchY(kFootPanel, At(v, stride, i, l.pos)[1]);
        }
    }
    if (done.size() > 4096) done.clear();
    done[v] = At(v, stride, 0, l.pos)[1];
}

// ---------------------------------------------------------------------------------------------------------------
// driving.xbe's pause menu (driving and on-rails missions): the same MISSION page, but each letter its own draw of a
// 256x256 sheet (a triangle fan of 4: float4 positions in stream 0, a colour in stream 1, coordinates in stream 2),
// in 640x480 space: Continue, Restart and Quit Mission with tops 178, 204 and 230 (shadows, 2 below and right of
// the letters), bottoms of the letters on the line at 195, 221 and 247; the >> marker a 10x13 quad at x 88 of
// another 256x256 sheet, at 178 + 25.5 a row. Controls and PC Settings go under Quit Mission, drawn by the game's own
// text routine (src/driving/pause.cpp), which also moves the rows and the marker up and tells this file what the menu
// shows (DrivingPauseShown, DrivingPauseMarker).

// driving.xbe's rows and marker go this far up (console pixels), as kPauseLift on foot
constexpr float kDriveLift = 22;
// the rows' tops (the letters) once moved up, and the step between them: the mouse's boxes (Ui)
[[maybe_unused]] constexpr float kDriveRowsTop = 178 - kDriveLift, kDriveStep = 26;
std::atomic<uint64_t> g_drive_seen{0};
std::atomic<uint64_t> g_drive_marker_seen{0};  // the >> marker: only the MISSION page has it
// the MISSION page, not another page whose words happen to fall on its rows' lines (the briefing)
bool DriveMissionPage() { return Recent(g_drive_seen.load()) && Recent(g_drive_marker_seen.load()); }
std::atomic<int> g_drive_lit{-1};

bool PromptQuad(const gpu::State& s, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                float alpha);
void LabelQuad(float x0, float y0, float x1, float y1);
uint64_t StageKey(const gpu::State& s);

// the stream i vertices of the draw whose stream 0 vertices are v
uint8_t* StreamOf(const gpu::State& s, const uint8_t* v, int i) {
    const auto& s0 = s.streams[0];
    const auto& si = s.streams[size_t(i)];
    if (!s0.vb || !s0.stride || !si.vb || !si.stride) return nullptr;
    uint32_t base0 = mem::VirtualFromPhysical(kernel::R32(s0.vb + 4));
    const uint8_t* h0 = static_cast<const uint8_t*>(kernel::HostPtr(base0));
    if (v < h0) return nullptr;
    size_t start = size_t(v - h0) / s0.stride;
    if (start > 0x10000) return nullptr;
    uint32_t base = mem::VirtualFromPhysical(kernel::R32(si.vb + 4)) + uint32_t(start) * si.stride;
    return static_cast<uint8_t*>(kernel::HostPtr(base));
}

uint32_t Channels(uint32_t c) { return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF); }

void DrivingPauseDraw(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    constexpr uint32_t kFan = 6;
    if (prim != kFan || count != 4 || stride < 16) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < 4; i++) {
        const float* p = At(v, stride, i, 0);
        x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
    }
    bool page = PausePage();
    auto collapse = [&] {
        for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[0] = x0, At(v, stride, i, 0)[1] = y0;
    };
    // a pause page up: the panel without its words and figure, taller (the rows and the marker are not drawn at all
    // then, src/driving/pause.cpp)
    // the running figure (a 128x128 picture right of the rows, reaching under the panel)
    if (page && (Paused() || Closing()) && TextureSize(s, 128, 128) && x0 > 450 && y0 > 150 && y0 < kDrivePanel.s1) {
        collapse();
        return;
    }
    if (page && (Paused() || Closing()) && InPanel(kDrivePanel, x0, y0, y1)) {
        bool word = false;
        if (KeepsSize(x1 - x0, y1 - y0) && y0 < kDriveWordsEnd && TextureSize(s, 256, 256)) {
            uint64_t key = StageKey(s);
            word = key == 0x03fc5f1b2a199c51ull || key == 0x3a69e5569f788462ull || key == 0xf3b5a8f28d4357d7ull;
        }
        if (word) {
            collapse();
            return;
        }
        // the quad's vertices are the game's of this frame (each draw its own buffer): moved once
        if (KeepsSize(x1 - x0, y1 - y0)) {
            float dy = StretchY(kDrivePanel, (y0 + y1) / 2) - (y0 + y1) / 2;
            for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[1] += dy;
            y0 += dy, y1 += dy;
        } else {
            for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[1] = StretchY(kDrivePanel, At(v, stride, i, 0)[1]);
            y0 = StretchY(kDrivePanel, y0), y1 = StretchY(kDrivePanel, y1);
        }
    }
    // a button prompt (coordinates in stream 2)
    if (const uint8_t* tc = StreamOf(s, v, 2); tc && s.streams[2].stride >= 8) {
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (uint32_t i = 0; i < 4; i++) {
            const float* t = reinterpret_cast<const float*>(tc + i * s.streams[2].stride);
            u0 = std::min(u0, t[0]), u1 = std::max(u1, t[0]), v0 = std::min(v0, t[1]), v1 = std::max(v1, t[1]);
        }
        float alpha = 1;
        if (const uint8_t* col = StreamOf(s, v, 1); col && s.streams[1].stride >= 4) {
            uint32_t c;
            std::memcpy(&c, col, 4);
            alpha = float(c >> 24) / 255.0f;
        }
        if (PromptQuad(s, x0, y0, x1, y1, u0, v0, u1, v1, alpha)) {
            collapse();
            return;
        }
    }
    if (!TextureSize(s, 256, 256)) return;
    // the HUD's sheet (the aperture, the weapon bar's tiles): each picture's edge half a texel in, so the upscaled
    // sheet's filtering does not take in the picture beside it (a line along the aperture) or the transparent texels
    // round a tile (a seam between the bar's tiles). Done the same way however often the game draws the quad.
    if (uint8_t* tc = StreamOf(s, v, 2); tc && s.streams[2].stride >= 8 && StageKey(s) == 0x3651421b66b89c6aull) {
        float* t[4];
        for (uint32_t i = 0; i < 4; i++) t[i] = reinterpret_cast<float*>(tc + i * s.streams[2].stride);
        for (int c = 0; c < 2; c++) {
            float lo = 1e9f, hi = -1e9f;
            for (auto* q : t) lo = std::min(lo, q[c] * 256), hi = std::max(hi, q[c] * 256);
            if (hi - lo < 4) continue;
            float base_lo = std::floor(lo + 0.25f), base_hi = std::ceil(hi - 0.25f);
            for (auto* q : t) {
                float x = q[c] * 256;
                q[c] = (x - lo < hi - x ? base_lo + 0.5f : base_hi - 0.5f) / 256;
            }
        }
    }
    // the lettering's black shadow copy (2 down and right): most of the way under the letters, fainter, so it no
    // longer reads as a line along them at PC resolutions
    if (uint8_t* col = StreamOf(s, v, 1); col && s.streams[1].stride >= 4) {
        uint64_t key = StageKey(s);
        if (key == 0x03fc5f1b2a199c51ull || key == 0x3a69e5569f788462ull || key == 0xf3b5a8f28d4357d7ull) {
            uint32_t c;
            std::memcpy(&c, col, 4);
            // moved once only (a quad the game draws again keeps the mark: blue 1, green 2)
            if (Channels(c) < 60 && (c & 0xFFFFu) != 0x0201u) {
                constexpr float kPull = 1.4f;
                for (uint32_t i = 0; i < 4; i++) {
                    At(v, stride, i, 0)[0] -= kPull, At(v, stride, i, 0)[1] -= kPull;
                    uint8_t* ci = col + i * s.streams[1].stride;
                    uint32_t ca;
                    std::memcpy(&ca, ci, 4);
                    ca = (ca & 0x00FF0000u) | 0x0201u | (uint32_t(((ca >> 24) * 3) / 5) << 24);
                    std::memcpy(ci, &ca, 4);
                }
                x0 -= kPull, x1 -= kPull, y0 -= kPull, y1 -= kPull;
            }
        }
    }
    // a visible letter (a prompt's words, perhaps): not a shadow, not see-through
    if (y1 - y0 < 40 && x1 - x0 < 40) {
        if (const uint8_t* lc = StreamOf(s, v, 1)) {
            uint32_t c;
            std::memcpy(&c, lc, 4);
            if ((c >> 24) >= 0x80 && Channels(c) >= 60) LabelQuad(x0, y0, x1, y1);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Button prompts: while the keyboard and mouse play, the menus' A, B and direction pictures make way for keyboard
// keys (ENTER, BACKSPACE, the arrows), which the pages draw where the game's were (Ui). The pictures are told by
// their texture's pack key and the part of it drawn.

enum class Prompt : uint8_t { A, B, UpDown, LeftRight };
struct PromptPart {
    uint64_t key;
    float u0, v0, u1, v1;  // texels of the sheet, -1 for the whole texture
    Prompt prompt;
};
constexpr PromptPart kPrompts[] = {
    // bond_eng.xbe: the A and B buttons (32x64 each), the D-pad pictures and the stick pictures in the button sheet
    {0x94de8205578ce49full, -1, -1, -1, -1, Prompt::A},
    {0x08e4f0ae95c16b80ull, -1, -1, -1, -1, Prompt::B},
    {0x7ffff33c7f8a7768ull, 225, 89, 248, 112, Prompt::UpDown},
    {0x7ffff33c7f8a7768ull, 125, 89, 147, 112, Prompt::LeftRight},
    // the pause menu's stick pictures (32x32): CHANGE PAGE and SCROLL
    {0x7e8cc44b3a0258fcull, 16, 1, 32, 16, Prompt::LeftRight},
    {0x7e8cc44b3a0258fcull, 16, 17, 32, 32, Prompt::UpDown},
    // driving.xbe's pause menu (a 256x256 sheet): A, B, CHANGE PAGE and SCROLL
    {0xa0aa68c1c1fc6b94ull, 99, 100, 131, 164, Prompt::A},
    {0xa0aa68c1c1fc6b94ull, 99, 34, 131, 98, Prompt::B},
    {0xa0aa68c1c1fc6b94ull, 202, 150, 218, 165, Prompt::LeftRight},
    {0xa0aa68c1c1fc6b94ull, 202, 176, 218, 191, Prompt::UpDown},
};

struct PromptRect {
    Prompt prompt;
    float x0, y0, x1, y1;  // console coordinates
    float alpha = 1;       // the picture's own (a menu fading out)
};
std::mutex g_prompt_lock;
// the prompts of the last few frames, each with its frame (Frame() while it was drawn): the keys are drawn over the
// frame being shown with that frame's prompts, none when it had none (a screen gone black, a loading screen)
struct FramePrompts {
    uint64_t frame = ~0ull;
    std::vector<PromptRect> list;
};
std::array<FramePrompts, 4> g_prompt_frames;
std::atomic<uint64_t> g_prompts_seen{0};
// driving.xbe quitting a mission: the pause screen's last picture stays up while the screen fades to black, its prompts
// not drawn again (src/driving/hooks.cpp); the prompts it showed are kept and fade with it
std::vector<PromptRect> g_prompts_held;
std::atomic<uint64_t> g_hold_seen{0};
std::atomic<float> g_fade{1};  // what the fade to black leaves (1 none, 0 black)

bool Keyboard() { return controls::ActiveDevice() == controls::Device::KeyboardMouse; }

// the title screen's "Press START" line as the game last drew it: its box, its first letter's height, its colour
struct PressLine {
    float x0, y0, x1, y1, first_h;
    std::array<float, 4> color;
};
PressLine g_press{};
std::atomic<uint64_t> g_press_seen{0};

// the pack key of the texture on stage 0, kept per texture and its bytes' place (hashed once)
uint64_t StageKey(const gpu::State& s) {
    uint32_t header = s.textures[0];
    if (!header) return 0;
    gpu::TextureDesc d;
    if (!gpu::Describe(header, d) || !d.data) return 0;
    static std::mutex lock;
    static std::unordered_map<uint64_t, uint64_t> cache;
    uint64_t id = uint64_t(header) << 32 | d.data;
    std::lock_guard l(lock);
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    uint32_t colours[256] = {};
    constexpr uint32_t kFmtP8 = 0x0B;
    if (d.format == kFmtP8 && s.palettes[0]) {
        uint32_t ph = s.palettes[0];
        uint32_t va = mem::VirtualFromPhysical(kernel::R32(ph + 4));
        size_t bytes = (256u >> (kernel::R32(ph) >> 30)) * 4;
        std::memcpy(colours, kernel::HostPtr(va), bytes);
    }
    // the texture pack's key (texturedumper::Key): the level's bytes over its shape, then a P8 texture's palette
    uint32_t shape[] = {d.format, d.width, d.height};
    uint64_t key = gpu::Hash(kernel::HostPtr(d.data), gpu::LevelSize(d, 0), gpu::Hash(shape, sizeof(shape)));
    if (d.format == kFmtP8) key = gpu::Hash(colours, sizeof(colours), key);
    if (!key) key = 1;
    if (cache.size() > 4096) cache.clear();
    cache[id] = key;
    return key;
}

// a prompt picture drawn: kept for the pages to draw the key over, true when the game's should go
bool PromptQuad(const gpu::State& s, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                float alpha) {
    if (x1 - x0 > 48 || y1 - y0 > 48 || x1 <= x0 || y1 <= y0) return false;
    uint64_t key = StageKey(s);
    if (!key) return false;
    gpu::TextureDesc d;
    gpu::Describe(s.textures[0], d);
    float tu0 = u0 * float(d.width), tu1 = u1 * float(d.width), tv0 = v0 * float(d.height), tv1 = v1 * float(d.height);
    for (const PromptPart& p : kPrompts) {
        if (p.key != key) continue;
        if (p.u0 >= 0 && !(Near(tu0, p.u0, 2) && Near(tu1, p.u1, 2) && Near(tv0, p.v0, 2) && Near(tv1, p.v1, 2))) continue;
        std::lock_guard l(g_prompt_lock);
        uint64_t f = Frame();
        FramePrompts& fp = g_prompt_frames[f % g_prompt_frames.size()];
        if (fp.frame != f) fp.frame = f, fp.list.clear();
        if (fp.list.size() < 64) fp.list.push_back({p.prompt, x0, y0, x1, y1, std::clamp(alpha, 0.0f, 1.0f)});
        g_prompts_seen.store(f + 1);
        return Keyboard();
    }
    return false;
}

// driving.xbe: the letters drawn, so a prompt with no words beside it (the pause menu's lone B) can go
struct LabelRect {
    float x0, y0, x1, y1;
};
std::vector<LabelRect> g_labels_building, g_labels_shown;
uint64_t g_labels_frame = ~0ull;

void LabelQuad(float x0, float y0, float x1, float y1) {
    std::lock_guard l(g_prompt_lock);
    uint64_t f = Frame();
    if (f != g_labels_frame) {
        g_labels_shown.swap(g_labels_building);
        g_labels_building.clear();
        g_labels_frame = f;
    }
    if (g_labels_building.size() < 4096) g_labels_building.push_back({x0, y0, x1, y1});
}

// words start just right of the prompt, on its line: true, and their middle line in *mid
bool Labelled(const PromptRect& p, float* mid = nullptr) {
    std::lock_guard l(g_prompt_lock);
    const auto& list = g_labels_shown.empty() ? g_labels_building : g_labels_shown;
    float cy = (p.y0 + p.y1) / 2;
    for (const LabelRect& r : list)
        if (r.x0 >= p.x1 - 2 && r.x0 <= p.x1 + 30 && r.y0 < cy && r.y1 > cy && r.y1 - r.y0 > 4) {
            if (mid) *mid = (r.y0 + r.y1) / 2;
            return true;
        }
    return false;
}

// the frame being shown has this seen mark (a frame number plus one, as the draws store them): the overlays that
// follow the game's own pictures (the keys over the prompts, the controls over the pre-mission Controller Setup) are
// drawn over the frames that have those pictures and no other
[[maybe_unused]] bool InShownFrame(uint64_t seen) { return seen && seen == app::g_shown_frame.load() + 1; }

// the prompts of the frame being shown
[[maybe_unused]] std::vector<PromptRect> ShownPrompts() {
    std::lock_guard l(g_prompt_lock);
    uint64_t shown = app::g_shown_frame.load();
    const FramePrompts& fp = g_prompt_frames[shown % g_prompt_frames.size()];
    if (fp.frame == shown && !fp.list.empty()) {
        g_prompts_held = fp.list;
        return fp.list;
    }
    // quitting a driving mission: the last ones, as the screen fades
    if (!Recent(g_hold_seen.load())) return {};
    std::vector<PromptRect> held = g_prompts_held;
    for (PromptRect& p : held) p.alpha *= g_fade.load();
    return held;
}

// ---------------------------------------------------------------------------------------------------------------
// pages


enum class Nav { Up, Down, Left, Right, Accept, Back, PrevTab, NextTab };
std::mutex g_nav_lock;
std::deque<Nav> g_nav;

void Push(Nav n) {
    std::lock_guard lock(g_nav_lock);
    if (g_nav.size() < 32) g_nav.push_back(n);
}

std::atomic<bool> g_opened{false};  // a page opened since the pages last drew

void Open(Page p) {
    g_opened.store(true);
    {
        std::lock_guard lock(g_nav_lock);
        g_nav.clear();
    }
    g_page.store(p);
}

// the pad's menu buttons, held
struct Held {
    bool up, down, left, right, a, b, lt, rt, start;
};

Held Read(const input::Pad& p) {
    constexpr int kStick = 16000;
    constexpr uint8_t kPressed = 0x80;
    Held h{};
    h.up = (p.buttons & 1) || p.ly > kStick;
    h.down = (p.buttons & 2) || p.ly < -kStick;
    h.left = (p.buttons & 4) || p.lx < -kStick;
    h.right = (p.buttons & 8) || p.lx > kStick;
    // START accepts too in the front end; on the pause menu it only shuts the menu (PadInner)
    h.a = p.analog[0] > kPressed || ((p.buttons & 0x10) && !Paused());
    h.b = p.analog[1] > kPressed || (p.buttons & 0x20);  // and BACK goes back
    h.lt = p.analog[6] > kPressed || p.analog[5] > kPressed;
    h.rt = p.analog[7] > kPressed || p.analog[4] > kPressed;
    h.start = p.buttons & 0x10;
    return h;
}

// presses, with held directions repeating
struct Edges {
    Held last{};
    int64_t held_since[4] = {}, repeated_at[4] = {};

    // which of up down left right fired this poll
    std::array<bool, 4> Directions(const Held& h) {
        constexpr int64_t kDelay = 400, kRate = 110;
        bool now[4] = {h.up, h.down, h.left, h.right}, before[4] = {last.up, last.down, last.left, last.right};
        std::array<bool, 4> fired{};
        int64_t t = NowMs();
        for (int i = 0; i < 4; i++) {
            if (now[i] && !before[i]) {
                fired[i] = true;
                held_since[i] = repeated_at[i] = t;
            } else if (now[i] && t - held_since[i] > kDelay && t - repeated_at[i] > kRate) {
                fired[i] = true;
                repeated_at[i] = t;
            }
        }
        return fired;
    }
};
Edges g_edges;

// inputs taken from the game stay away from it until let go, so it never sees them pressed: up down left right a b
uint32_t g_swallow = 0;
constexpr uint32_t kSwA = 16, kSwB = 32;

void Swallow(input::Pad& p, const Held& h) {
    bool held[6] = {h.up, h.down, h.left, h.right, h.a, h.b};
    for (int i = 0; i < 6; i++)
        if (!held[i]) g_swallow &= ~(1u << i);
    if (g_swallow & 1) p.buttons &= uint16_t(~1u), p.ly = std::min<int16_t>(p.ly, 0);
    if (g_swallow & 2) p.buttons &= uint16_t(~2u), p.ly = std::max<int16_t>(p.ly, 0);
    if (g_swallow & 4) p.buttons &= uint16_t(~4u), p.lx = std::max<int16_t>(p.lx, 0);
    if (g_swallow & 8) p.buttons &= uint16_t(~8u), p.lx = std::min<int16_t>(p.lx, 0);
    if (g_swallow & kSwA) p.analog[0] = 0, p.buttons &= uint16_t(~0x10u);
    if (g_swallow & kSwB) p.analog[1] = 0, p.buttons &= uint16_t(~0x20u);
}

input::Pad Neutral(const input::Pad& p) {
    input::Pad n;
    n.connected = p.connected;
    return n;
}

// a pad with one direction or button taken out
void Take(input::Pad& p, int dir) {
    constexpr uint16_t kBits[4] = {1, 2, 4, 8};
    p.buttons &= uint16_t(~kBits[dir]);
    if (dir < 2) p.ly = 0;
    else p.lx = 0;
    g_swallow |= 1u << dir;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------

void Draw(const void* state, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    if (kDrivingImage) {
        DrivingPauseDraw(*static_cast<const gpu::State*>(state), prim, v, stride, count);
        return;
    }
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    if ((Paused() || Closing()) && PausePage() && PausePanelDraw(s, prim, v, stride, count)) return;
    Layout l = MenuLayout(s, prim, stride, count);
    if (!l.ok) return;
    Box b = BoxOf(v, stride, l, 0, count);

    // the lettering's black shadow copy, a console pixel down and right: at PC resolutions that is a hard dark edge
    // along every letter (a line under and beside the text). It moves most of the way under the letters, fainter.
    // A line the game keeps (and draws again every frame) is moved once: its red is set to a mark that says so.
    constexpr float kMoved = 1.0f / 512;
    bool glyphs = count % 6 == 0;  // a line of letters: every quad letter-sized (not a dark picture)
    for (uint32_t q = 0; glyphs && q < count; q += 6) {
        Box g = BoxOf(v, stride, l, q, 6);
        glyphs = g.x1 - g.x0 < 48 && g.y1 - g.y0 < 48;
    }
    if (glyphs && TextureSize(s, 256, 128) && IsShadow(ColorOf(v, stride, l)) && ColorOf(v, stride, l)[0] != kMoved) {
        constexpr float kPull = 0.65f, kFade = 0.6f;
        for (uint32_t i = 0; i < count; i++) {
            float* p = At(v, stride, i, l.pos);
            p[0] -= kPull, p[1] -= kPull;
            float* c = At(v, stride, i, l.color);
            c[0] = kMoved;
            c[3] *= kFade;
        }
        b = BoxOf(v, stride, l, 0, count);
    }

    // the words beside a prompt: the key goes on their middle line (DrawPrompts)
    if (glyphs && count <= 6 * 40 && !IsShadow(ColorOf(v, stride, l)))
        for (uint32_t q = 0; q < count; q += 6) {
            Box g = BoxOf(v, stride, l, q, 6);
            LabelQuad(g.x0, g.y0, g.x1, g.y1);
        }

    // a button prompt: the keyboard's key in its place while the keyboard and mouse play
    if (count == 6 && PromptQuad(s, b.x0, b.y0, b.x1, b.y1, b.u0, b.v0, b.u1, b.v1, ColorOf(v, stride, l)[3])) {
        Hide(v, stride, l, count);
        return;
    }

    // the title screen's "Press START" (one line of 11 letter quads in the list's lettering, centred under the logo):
    // while the keyboard and mouse play it gives way to "Press ENTER", which the pages draw (Ui)
    if (count == 66 && Near((b.x0 + b.x1) / 2, 320, 40) && b.y0 > 280 && b.y1 < 360 && TextureSize(s, 256, 128) &&
        !Recent(g_main_seen.load()) && StageKey(s) == 0x2829182bc4c846c2ull) {
        Box first = BoxOf(v, stride, l, 0, 6);
        {
            std::lock_guard lock(g_prompt_lock);
            g_press = {b.x0, b.y0, b.x1, b.y1, first.y1 - first.y0, ColorOf(v, stride, l)};
        }
        g_press_seen.store(Frame() + 1);
        if (Keyboard()) Hide(v, stride, l, count);
        return;
    }

    if (count == 42 && b.x0 > 87 && b.x0 < 94 && b.y0 > 73 && b.y0 < 81 && TextureSize(s, 256, 128) &&
        StageKey(s) == 0x254f6da069f11c3full)
        g_options_title_seen.store(Frame() + 1);
    if (OptionsTitle(s, b)) {
        g_title_line.assign(v, v + 6 * stride);
        g_title_box = b;
        Hide(v, stride, l, count);
        return;
    }

    // the pre-mission Controller Setup: its pictures go
    if (SetupTitle(s, b, count)) {
        g_setup_seen.store(Frame() + 1);
        g_setup_frame = Frame();
    } else if (count == 6 && g_setup_frame == Frame() && b.x0 >= 40 && b.y0 >= 120 && b.y0 < 400 &&
               b.x1 <= 620 && b.y1 <= 470) {
        // every picture after the title, the labels and their lines too (the driving and rail pages use other
        // sheets than the on-foot one), above the footer
        Hide(v, stride, l, count);
        return;
    }

    // the pause menu
    if (Paused()) {
        bool page = PausePage();
        if (count == 6 && TextureSize(s, 64, 64) && Near(b.x0, kMarkerX, 3)) {
            // the marker is the << of a 64x64 sheet (drawn mirrored) whose quad starts on the texel row right under
            // the sheet's blue picture and ends at its right edge: the filtering took in a row of that picture (a
            // line along the marker's top) and, wrapping, the sheet's left edge. Each edge of the part drawn goes
            // half a texel in, the same way however often the game draws it (as the driving HUD's sheet).
            for (int c = 0; c < 2; c++) {
                float lo = 1e9f, hi = -1e9f;
                for (uint32_t i = 0; i < 6; i++) {
                    float x = At(v, stride, i, l.tc)[c] * 64;
                    lo = std::min(lo, x), hi = std::max(hi, x);
                }
                if (hi - lo < 4) continue;
                float base_lo = std::floor(lo + 0.25f), base_hi = std::ceil(hi - 0.25f);
                for (uint32_t i = 0; i < 6; i++) {
                    float* q = At(v, stride, i, l.tc);
                    float x = q[c] * 64;
                    q[c] = (x - lo < hi - x ? base_lo + 0.5f : base_hi - 0.5f) / 64;
                }
            }
            int extra = g_pause_extra.load();
            if (page) Hide(v, stride, l, count);
            else MoveY(v, stride, l, count, PauseMarkerTarget(b.y0, extra) - b.y0);
            return;
        }
        bool lifted = false;
        int row = PauseRow(s, b, &lifted);
        if (row >= 0) {
            if (!lifted) MoveY(v, stride, l, count, -kPauseLift);
            auto color = ColorOf(v, stride, l);
            if (!IsShadow(color)) {
                if (row == kPauseRows - 1) g_pause_seen.store(Frame() + 1);
                if (Brightness(color) > Brightness(g_pause_unlit_color) + 0.3f || Brightness(color) > 2.4f) {
                    g_pause_lit.store(row);
                    g_pause_lit_color = color;
                } else {
                    g_pause_unlit_color = color;
                }
                if (g_pause_extra.load() >= 0 && row == g_pause_lit.load()) SetColor(v, stride, l, count, g_pause_unlit_color);
            }
            if (page) Hide(v, stride, l, count);
            return;
        }
    }

    if (count == 6) {
        // a main menu bar: its row moved up, and OPTIONS unlit while QUIT GAME is selected
        int band = BarBand(s, b);
        if (band >= 0) {
            int item = band % kMainItems;
            if (band < kMainItems) g_main_selected.store(item);
            g_main_seen.store(Frame() + 1);
            MoveY(v, stride, l, count, kNewTop + kNewStep * float(item) - kMainTops[item]);
            if (item == g_quit_game_row.load() && band < kMainItems && g_quit_selected.load()) SetBand(v, stride, l, count, band, band + kMainItems);
            return;
        }
        // an options slider (the button sheet's bar, right of the values)
        if (Recent(g_options_seen.load()) && Near(b.x0, 350, 4) && b.y0 > 150 && b.y1 < 400 &&
            TextureSize(s, 256, 256)) {
            if (g_page.load() != Page::None) Hide(v, stride, l, count);
            else MoveY(v, stride, l, count, kInsert);
            return;
        }
    }

    bool value = false;
    int row = OptionRow(s, b, value);
    if (row < 0) return;
    auto color = ColorOf(v, stride, l);
    bool shadow = IsShadow(color);
    // only under the OPTIONS title: other screens draw lines of the same lettering where the list's rows are (the
    // letters learnt from them were wrong ones, and PC Settings came out as other letters)
    if (g_options_title_seen.load() != Frame() + 1) return;
    if (!value && !shadow) {
        Learn(StageKey(s), v, stride, l, count, kLabels[row]);
        if (row == kControllerRow && count == std::strlen(kLabels[row]) * 6) g_options_seen.store(Frame() + 1);
        if (Brightness(color) > Brightness(g_unlit_color) + 0.3f) {
            g_options_selected.store(row);
            g_lit_color = color;
        } else {
            g_unlit_color = color;
        }
    } else if (value && !shadow && row != g_options_selected.load()) {
        g_unlit_value_color = color;
    }
    if (!Recent(g_options_seen.load())) return;
    if (g_page.load() != Page::None) {
        Hide(v, stride, l, count);  // the page lists its own rows here
        return;
    }
    // the row the game has lit looks unlit while PC Settings is selected
    if (!shadow && g_pc_selected.load() && row == g_options_selected.load()) SetColor(v, stride, l, count, value ? g_unlit_value_color : g_unlit_color);
    if (row > kControllerRow) MoveY(v, stride, l, count, kInsert);
}

void AfterDraw(const void* state, uint32_t prim, const uint8_t* v, uint32_t stride, uint32_t count, app::GameInfo::Emit emit) {
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    Layout l = MenuLayout(s, prim, stride, count);
    if (!l.ok) return;
    Box b = BoxOf(v, stride, l, 0, count);

    // the page's name in place of OPTIONS
    if (!g_title_line.empty() && OptionsTitle(s, b)) {
        const char* name = g_page.load() == Page::Controls ? "CONTROLLER SETUP" : "PC SETTINGS";
        std::vector<uint8_t> text = ItalicText(name, g_title_line.data(), stride, l, g_title_box.x0, g_title_box.y0,
                                               ColorOf(g_title_line.data(), stride, l));
        g_title_line.clear();
        if (!text.empty()) emit(prim, text.data(), uint32_t(text.size() / stride));
        return;
    }

    // Controls and PC Settings under Quit Mission, shadow and letters each after the game's own
    if (count >= 6 && Paused() && PauseRow(s, b) == kPauseRows - 1 && !PausePage()) {
        auto color = ColorOf(v, stride, l);
        bool shadow = IsShadow(color);
        Box first = BoxOf(v, stride, l, 0, 6);
        int extra = g_pause_extra.load();
        std::vector<uint8_t> all;
        for (int i = 0; i < kPauseExtraRows; i++) {
            auto c = shadow ? color : (extra == i ? g_pause_lit_color : g_pause_unlit_color);
            std::vector<uint8_t> text = ItalicText(kPauseExtra[i], v, stride, l, first.x0, first.y0 + kPauseStep * float(i + 1), c);
            all.insert(all.end(), text.begin(), text.end());
        }
        if (!all.empty()) emit(prim, all.data(), uint32_t(all.size() / stride));
        return;
    }

    if (count == 6) {
        int band = BarBand(s, b);
        if (band >= 0 && band % kMainItems == kMainItems - 1) {
            // QUIT GAME, a row under OPTIONS (already moved), from the sheet's added bands
            std::vector<uint8_t> bar(v, v + 6 * stride);
            int to = g_quit_selected.load() ? kQuitLit : kQuitUnlit;
            int from = band;
            if (g_quit_selected.load() && band < kMainItems) from = band;  // Draw unlit it in the game's memory already
            for (uint32_t i = 0; i < 6; i++) {
                float* t = reinterpret_cast<float*>(bar.data() + i * stride + l.tc);
                float* p = reinterpret_cast<float*>(bar.data() + i * stride + l.pos);
                t[1] += float(to) * kBand - b.v0;
                p[1] += kNewStep;
            }
            (void)from;
            emit(prim, bar.data(), 6);
        }
        return;
    }

    if (g_page.load() != Page::None) return;
    bool value = false;
    int row = OptionRow(s, b, value);
    if (row != kControllerRow || value || count != std::strlen(kLabels[row]) * 6) return;
    if (!Recent(g_options_seen.load())) return;
    auto color = ColorOf(v, stride, l);
    if (!IsShadow(color)) color = g_pc_selected.load() ? g_lit_color : g_unlit_color;
    Box first = BoxOf(v, stride, l, 0, 6);
    std::vector<uint8_t> text = Text(StageKey(s), kPcSettings, v, stride, l, first.x0, first.y0 + kInsert, color);
    if (!text.empty()) emit(prim, text.data(), uint32_t(text.size() / stride));
}

bool PageOpen() { return g_page.load() != Page::None; }

void DrivingPauseShown(int lit) {
    g_drive_seen.store(Frame() + 1);
    g_drive_lit.store(lit);
}
void DrivingPauseMarker() { g_drive_marker_seen.store(Frame() + 1); }
bool PausePageUp() { return PausePage(); }
int PauseExtraSelected() { return g_pause_extra.load(); }
int PauseExtraRows() { return kPauseExtraRows; }
const char* PauseExtraLabel(int row) { return row >= 0 && row < kPauseExtraRows ? kPauseExtra[row] : ""; }
float DrivingPauseLift() { return kDriveLift; }

void ScreenFade(float left) { g_fade.store(std::clamp(left, 0.0f, 1.0f)); }
void HoldPrompts() {
    if (!Recent(g_hold_seen.load())) g_fade.store(1);
    g_hold_seen.store(Frame() + 1);
}

void SetMissionQuery(controls::Context (*query)()) { g_mission_query = query; }

bool TakePauseClose() {
    int64_t at = g_close_pause_at.exchange(0);
    // a request the game's menu tick never came for (the menu shut meanwhile) goes stale
    return at && NowMs() - at < 1000;
}

void PadInner(int port, input::Pad& pad, const Held& h);

namespace {

// ---------------------------------------------------------------------------------------------------------------
// The mouse on the game's own menus (the main menu, the options list, the pause menus' MISSION page): the row under
// the cursor is selected as the cursor moves, and a click on it accepts it. Ui finds the row (MouseMenus, the rows'
// boxes known here); the pad then gets D-pad presses toward it, one row a press, and A once there, as a player would
// press them, so the added rows (QUIT GAME, PC Settings, Controls) work through the same code as the controller's.

enum class MouseMenu { None, Main, Options, Pause };
std::atomic<int> g_mouse_menu{0};       // a MouseMenu
std::atomic<int> g_mouse_target{-1};    // the row to go to, -1 none
std::atomic<bool> g_mouse_accept{false};  // and A there
std::atomic<bool> g_mouse_strip_a{false};  // the click's own A held back until the row is reached
std::atomic<int64_t> g_mouse_at{0};

// the menu up and its selected row (the main menu 0 to 5 with QUIT GAME; the options list 0 to 10 with PC Settings
// third; the pause menu 0 to 4 with Controls and PC Settings), -1 when that menu is not up
int MouseRowNow(MouseMenu m) {
    switch (m) {
        case MouseMenu::Main:
            if (!Recent(g_main_seen.load()) || g_page.load() != Page::None) return -1;
            return g_quit_selected.load() ? kMainItems : g_main_selected.load();
        case MouseMenu::Options: {
            if (!Recent(g_options_seen.load()) || g_page.load() != Page::None) return -1;
            int sel = g_options_selected.load();
            if (g_pc_selected.load()) return kControllerRow + 1;
            return sel < 0 ? -1 : sel <= kControllerRow ? sel : sel + 1;
        }
        case MouseMenu::Pause: {
            if (!Paused() || g_page.load() != Page::None) return -1;
            bool up = kDrivingImage ? DriveMissionPage() : Recent(g_pause_seen.load());
            if (!up) return -1;
            int extra = g_pause_extra.load();
            if (extra >= 0) return kPauseRows + extra;
            return kDrivingImage ? g_drive_lit.load() : g_pause_lit.load();
        }
        default: return -1;
    }
}

void MouseSteer(input::Pad& pad) {
    static int phase = 0, accepting = 0;
    int target = g_mouse_target.load();
    bool strip = g_mouse_strip_a.load();
    if (strip) {
        // held back until the row is reached and accepted, then until the button is let go
        if (target < 0 && !accepting && pad.analog[0] == 0) g_mouse_strip_a.store(false);
        pad.analog[0] = 0;
    }
    if (accepting) {
        pad.analog[0] = 0xFF;
        accepting--;
        return;
    }
    if (target < 0) {
        phase = 0;
        return;
    }
    int cur = MouseRowNow(MouseMenu(g_mouse_menu.load()));
    if (cur < 0 || NowMs() - g_mouse_at.load() > 4000) {
        g_mouse_target.store(-1), g_mouse_accept.store(false);
        return;
    }
    if (cur == target) {
        g_mouse_target.store(-1);
        if (g_mouse_accept.exchange(false)) accepting = 3;
        phase = 0;
        return;
    }
    // a press toward it, held two polls and let go four: the selection the menus' draws report has caught up by the
    // next one
    if (phase < 2) pad.buttons |= target > cur ? 2 : 1;
    phase = (phase + 1) % 6;
}

}  // namespace

void Pad(int port, input::Pad& pad) {
    if (!(kMenus || kDrivingImage) || port != 0) return;
    MouseSteer(pad);
    Held h = Read(pad);
    PadInner(port, pad, h);
    // what the game was kept from stays kept from it until let go
    bool neutral = pad.buttons == 0 && pad.lx == 0 && pad.ly == 0 && pad.analog[0] == 0 && pad.analog[1] == 0;
    if (neutral) {
        bool held[6] = {h.up, h.down, h.left, h.right, h.a, h.b};
        for (int i = 0; i < 6; i++)
            if (held[i]) g_swallow |= 1u << i;
    }
    Swallow(pad, h);
}

void PadInner(int, input::Pad& pad, const Held& h) {
    std::array<bool, 4> dirs = g_edges.Directions(h);
    bool a = h.a && !g_edges.last.a, back = h.b && !g_edges.last.b;
    bool lt = h.lt && !g_edges.last.lt, rt = h.rt && !g_edges.last.rt;
    bool start = h.start && !g_edges.last.start;
    g_edges.last = h;

    // START (ESCAPE on the keyboard) only opens and shuts the pause menu: from any page, row or question of it
    // (the game's own and this port's Controls) it shuts the lot, and it never selects. The game's own pause menus
    // take START as A: bond_eng.xbe never sees it while paused, the menu shuts as Continue Mission does
    // (TakePauseClose, src/game/pause.cpp); driving.xbe gets it and turns it into Continue Mission
    // (src/driving/pause.cpp).
    // (bond_eng.xbe's split screen games, [0x205408] players, have a pause screen a player and keep the game's way.)
    bool esc = g_escape_close.exchange(false);
    bool own = kDrivingImage || kernel::R32(0x205408) == 1;
    // a pause page asked to close with the pause menu: it goes once the game has shut the menu (or after 1.5 s, should
    // the game never do so), the game held back meanwhile
    if (int64_t at = g_closing_at.load()) {
        // the game draws its pause menu once more in the frame it shuts it: the page stays until that has gone by
        uint64_t seen = kDrivingImage ? g_drive_seen.load() : g_pause_seen.load();
        bool drawn = seen && Frame() + 1 - seen <= 1;
        if ((!Paused() && !drawn) || NowMs() - at > 1500) {
            g_closing_at.store(0);
            g_page.store(Page::None);
            g_pause_extra.store(-1);
        } else {
            // driving.xbe takes START as Continue Mission (src/driving/pause.cpp): held for its first polls, as a
            // press is, and for as long as it is held
            input::Pad out = Neutral(pad);
            if (kDrivingImage && (NowMs() - at < 100 || h.start)) out.buttons |= 0x10;
            pad = out;
            return;
        }
    }
    if (Paused() && own) {
        if (start || esc) {
            // from a page: it stays up until the menu has shut (no frame of the pause menu on the way out)
            if (PausePage()) g_closing_at.store(NowMs());
            else g_page.store(Page::None), g_pause_extra.store(-1);
            input::Pad out = Neutral(pad);
            if (kDrivingImage) out.buttons |= 0x10;
            else g_close_pause_at.store(NowMs());
            pad = out;
            return;
        }
        if (!kDrivingImage) pad.buttons &= uint16_t(~0x10u);
    } else if (PausePage() && (start || esc)) {
        g_page.store(Page::None);
        g_pause_extra.store(-1);
        input::Pad out = Neutral(pad);
        out.buttons |= 0x10;
        pad = out;
        return;
    }

    if (g_page.load() != Page::None) {
        if (dirs[0]) Push(Nav::Up);
        if (dirs[1]) Push(Nav::Down);
        if (dirs[2]) Push(Nav::Left);
        if (dirs[3]) Push(Nav::Right);
        if (a) Push(Nav::Accept);
        if (back) Push(Nav::Back);
        if (lt) Push(Nav::PrevTab);
        if (rt) Push(Nav::NextTab);
        pad = Neutral(pad);
        return;
    }

    // the pause menu: Controls and PC Settings past Quit Mission
    bool pause = (kDrivingImage ? DriveMissionPage() : Recent(g_pause_seen.load())) && Paused();
    int lit = kDrivingImage ? g_drive_lit.load() : g_pause_lit.load();
    if (!pause) g_pause_extra.store(-1);
    if (pause) {
        int extra = g_pause_extra.load();
        if (extra < 0) {
            if (dirs[1] && lit == kPauseRows - 1) {
                g_pause_extra.store(0);
                Take(pad, 1);
            }
        } else {
            // the game keeps Quit Mission lit underneath: up from the first added row hands the selection back to it
            input::Pad out = Neutral(pad);
            if (dirs[0]) g_pause_extra.store(extra - 1);
            else if (dirs[1]) g_pause_extra.store(std::min(extra + 1, kPauseExtraRows - 1));
            else if (a && !start) Open(extra == 0 ? Page::PauseControls : Page::PausePcSettings);
            out.analog[1] = pad.analog[1];      // B and START still close the pause menu
            out.buttons |= pad.buttons & 0x10;
            pad = out;
        }
        return;
    }

    // the main menu: QUIT GAME past OPTIONS
    bool main = Recent(g_main_seen.load());
    if (!main) g_quit_selected.store(false);
    if (main) {
        int item = g_main_selected.load();
        if (!g_quit_selected.load()) {
            if (dirs[1] && item == kMainItems - 1) {
                g_quit_selected.store(true);
                g_quit_game_row.store(item);
                Take(pad, 1);
            } else if (dirs[0] && item == 0) {
                // up from START MISSION wraps to QUIT GAME at the bottom
                g_quit_selected.store(true);
                g_quit_game_row.store(0);
                Take(pad, 0);
            }
        } else {
            // the game wraps itself, so a press away is handed on only when it lands where it should
            int under = g_quit_game_row.load();
            input::Pad out = Neutral(pad);
            if (dirs[0]) {
                g_quit_selected.store(false);
                if (under == 0) out.buttons |= 1;               // Start Mission wraps up to Options
            } else if (dirs[1]) {
                g_quit_selected.store(false);
                if (under == kMainItems - 1) out.buttons |= 2;  // Options wraps down to Start Mission
            } else if (a) {
                Open(Page::Quit);
            }
            pad = out;
        }
        return;
    }

    // the options list: PC Settings between Controller Setup and Invert Pitch, and Controller Setup opening the
    // controls page
    bool options = Recent(g_options_seen.load());
    if (!options) {
        g_pc_selected.store(false);
        return;
    }
    int sel = g_options_selected.load();
    if (!g_pc_selected.load()) {
        if (dirs[1] && sel == kControllerRow) {
            g_pc_selected.store(true);
            g_pc_game_row.store(sel);
            Take(pad, 1);
        } else if (dirs[0] && sel == kControllerRow + 1) {
            g_pc_selected.store(true);
            g_pc_game_row.store(sel);
            Take(pad, 0);
        } else if (a && sel == kControllerRow) {
            Open(Page::Controls);
            pad = Neutral(pad);
        }
        return;
    }
    // PC Settings selected: the game keeps the row it had, and a press away hands it on
    int under = g_pc_game_row.load();
    input::Pad out = Neutral(pad);
    if (dirs[0]) {
        g_pc_selected.store(false);
        if (under != kControllerRow) out.buttons |= 1;  // the game goes up to Controller Setup
    } else if (dirs[1]) {
        g_pc_selected.store(false);
        if (under == kControllerRow) out.buttons |= 2;  // the game goes down to Invert Pitch
    } else if (a) {
        Open(Page::PcSettings);
    } else if (back) {
        // B still leaves the options for the main menu
        g_pc_selected.store(false);
        out.analog[1] = pad.analog[1];
    }
    if (!a && !dirs[0] && !dirs[1]) out.analog[1] = pad.analog[1];
    out.buttons |= pad.buttons & 0x20;  // and BACK
    pad = out;
}

}  // namespace menus

// ---------------------------------------------------------------------------------------------------------------
// the pages, drawn with ImGui over the game's own screen, in its places and colours

#ifndef _WIN32
namespace menus {
void Ui() {}
void Window(void*) {}
bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }
}  // namespace menus
#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cwctype>
#include <iterator>
#include <filesystem>

#include <imgui.h>

#include "common/menu_font.h"
#include "common/menus_pages.h"

#ifdef _MSC_VER
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace menus {

namespace {

ImFont* g_font = nullptr;  // the game's lettering (GameFont), else Bahnschrift
bool g_game_font = false;
ImFont* g_title_font = nullptr;  // the italic lettering, for titles and tabs (else g_font)

// console coordinates (640x480) to the window's
ImVec2 Guest(float x, float y) {
    const float* r = app::g_frame_rect;
    if (r[2] <= 0 || r[3] <= 0) {
        ImVec2 d = ImGui::GetIO().DisplaySize;
        return ImVec2(x * d.x / 640, y * d.y / 480);
    }
    return ImVec2(r[0] + x * r[2] / 640, r[1] + y * r[3] / 480);
}
float GuestScale() {
    const float* r = app::g_frame_rect;
    return r[3] > 0 ? r[3] / 480 : ImGui::GetIO().DisplaySize.y / 480;
}

std::vector<Nav> TakeNav() {
    std::lock_guard lock(g_nav_lock);
    std::vector<Nav> n(g_nav.begin(), g_nav.end());
    g_nav.clear();
    return n;
}

ImU32 Rgb(const std::array<float, 4>& c, float a = 1) {
    return IM_COL32(int(std::clamp(c[0], 0.f, 1.f) * 255), int(std::clamp(c[1], 0.f, 1.f) * 255),
                    int(std::clamp(c[2], 0.f, 1.f) * 255), int(a * 255));
}

// text at console coordinates, with the list's shadow a pixel down and right
// a line in the italic lettering's ImGui font, a letter at a time with ItalicAdvance's even gaps between the letters
// (ImGui's own layout has only each letter's advance); characters the table lacks keep the font's advance. Its width
// in window pixels when dl is null.
float ItalicLine(ImFont* f, ImDrawList* dl, float px, ImVec2 p, ImU32 col, const char* text) {
    float s = px / float(kTitleFont.line_height), x0 = p.x;
    char one[2] = {0, 0};
    for (const char* c = text; *c; c++) {
        one[0] = *c;
        if (dl) dl->AddText(f, px, p, col, one);
        if (FindGlyph(kTitleFont, *c) || *c == ' ') p.x += float(ItalicAdvance(*c, c[1])) * s;
        else p.x += f->GetCharAdvance(ImWchar(static_cast<unsigned char>(*c))) * px / f->FontSize;
    }
    return p.x - x0;
}

void SayIn(ImFont* f, ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align) {
    if (!f) f = g_font ? g_font : ImGui::GetFont();
    float px = size * GuestScale();
    ImVec2 p = Guest(x, y);
    bool italic = f == g_title_font && g_title_font;
    if (align != 0) p.x -= (italic ? ItalicLine(f, nullptr, px, p, col, text) : f->CalcTextSizeA(px, FLT_MAX, 0, text).x) * align;
    // no shadow copy: at PC resolutions its offset edge reads as a line along every letter
    if (italic) ItalicLine(f, dl, px, p, col, text);
    else dl->AddText(f, px, p, col, text);
}

void Say(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align = 0) {
    SayIn(g_font, dl, x, y, size, col, text, align);
}
void Title(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align) {
    SayIn(g_title_font, dl, x, y, size, col, text, align);
}
float WidthIn(ImFont* f, float size, const char* text) {
    if (!f) f = g_font ? g_font : ImGui::GetFont();
    float px = size * GuestScale();
    if (f == g_title_font && g_title_font) return ItalicLine(f, nullptr, px, ImVec2(0, 0), 0, text) / GuestScale();
    return f->CalcTextSizeA(px, FLT_MAX, 0, text).x / GuestScale();
}
float TitleWidth(float size, const char* text) { return WidthIn(g_title_font, size, text); }

float Width(float size, const char* text) {
    ImFont* f = g_font ? g_font : ImGui::GetFont();
    float px = size * GuestScale();
    return f->CalcTextSizeA(px, FLT_MAX, 0, text).x / GuestScale();
}

PageContext Context() {
    PageContext c;
    c.width = Width;
    c.guest = Guest;
    c.scale = GuestScale;
    c.say = Say;
    c.title = Title;
    c.title_width = TitleWidth;
    c.lit = Rgb(g_lit_color);
    c.unlit = Rgb(g_unlit_color);
    c.value = Rgb(g_unlit_value_color);
    if (kDrivingImage) c.pause_title_y = 72, c.pause_top = 106, c.pause_bar_y = 386;
    return c;
}

// ARE YOU SURE?  YES / NO, centred over the main menu
int g_quit_choice = 1;  // 0 yes, 1 no
void QuitPage(const std::vector<Nav>& nav) {
    for (Nav n : nav) {
        if (n == Nav::Left || n == Nav::Up) g_quit_choice = 0;
        if (n == Nav::Right || n == Nav::Down) g_quit_choice = 1;
        if (n == Nav::Back) {
            g_page.store(Page::None);
            return;
        }
        if (n == Nav::Accept) {
            if (g_quit_choice == 0) {
                LOGI("menus: QUIT GAME");
                PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
            }
            g_page.store(Page::None);
            g_quit_choice = 1;
            return;
        }
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 a = Guest(170, 190), b = Guest(470, 300);
    dl->AddRectFilled(a, b, IM_COL32(4, 10, 28, 235));
    dl->AddRect(a, b, IM_COL32(120, 150, 230, 200), 0, 0, std::max(1.0f, GuestScale()));
    Say(dl, 320, 210, 22, Rgb(g_lit_color), "QUIT GAME?", 0.5f);
    Say(dl, 320, 238, 14, Rgb(g_unlit_color), "Are you sure you want to quit?", 0.5f);
    Say(dl, 270, 266, 18, g_quit_choice == 0 ? Rgb(g_lit_color) : Rgb(g_unlit_color), "YES", 0.5f);
    Say(dl, 370, 266, 18, g_quit_choice == 1 ? Rgb(g_lit_color) : Rgb(g_unlit_color), "NO", 0.5f);
    // the mouse
    ImGuiIO& io = ImGui::GetIO();
    for (int c = 0; c < 2; c++) {
        ImVec2 p0 = Guest(c ? 340 : 240, 262), p1 = Guest(c ? 400 : 300, 290);
        if (ImGui::IsMouseHoveringRect(p0, p1, false)) {
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) g_quit_choice = c;
            if (ImGui::IsMouseClicked(0)) Push(Nav::Accept);
        }
    }
}

}  // namespace

// The game's own italic lettering for the pages, from the texture pack's image of its font sheet (any size, the
// sheet's 256x128 layout scaled): each character of menu_font.h goes into ImGui's atlas as a glyph of a font of its
// own. Without the image the pages fall back to Bahnschrift.
bool LoadSheet(const std::filesystem::path& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* src = nullptr;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &src);
    if (SUCCEEDED(hr)) hr = src->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w < 256 || h < 128 || w > 8192 || h > 4096 || w * 128 != h * 256)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        bgra.resize(size_t(w) * h * 4);
        hr = src->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data());
    }
    if (src) src->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    wic->Release();
    return SUCCEEDED(hr);
}

std::filesystem::path FindSheet(const char* font_key) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = app::ExeDir() / "textures" / "replacement";
    if (!fs::is_directory(root, ec)) return {};
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::wstring name = p.filename().wstring(), ext = p.extension().wstring();
        for (auto& ch : ext) ch = wchar_t(towlower(ch));
        std::string key = font_key;
        if (ext == L".png" && name.find(std::wstring(key.begin(), key.end())) != std::wstring::npos) return p;
    }
    return {};
}

// one font of the game's lettering on its way into ImGui's atlas: its sheet, and the atlas rectangles of its glyphs
struct SheetFont {
    const MenuFont* font = nullptr;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    float k = 1;  // sheet pixels a texel
    ImFont* im = nullptr;
    std::vector<int> ids;
    int space = -1;
    bool outline = false;  // the sheet's letters have a dark outline (FillSheetFont draws one round them)
};

// the sheet's white coverage at a pixel (0-255): an outline drawn dark round the letters is not part of them
inline int WhiteAt(const SheetFont& sf, size_t i) {
    const uint8_t* p = &sf.px[i * 4];
    return p[3] * std::max({p[0], p[1], p[2]}) / 255;
}

bool AddSheetFont(ImGuiIO& io, const MenuFont& mf, const char* name, SheetFont& sf) {
    sf.font = &mf;
    std::filesystem::path path = FindSheet(mf.key);
    if (path.empty() || !LoadSheet(path, sf.px, sf.w, sf.h)) {
        LOGI("menus: the font sheet %s is not in the texture pack, the pages use Bahnschrift for it", mf.key);
        return false;
    }
    float k = sf.k = float(sf.w) / 256.0f;
    ImFontConfig cfg;
    cfg.SizePixels = std::round(mf.line_height * k);
    cfg.GlyphRanges = io.Fonts->GetGlyphRangesDefault();
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    sf.im = io.Fonts->AddFontDefault(&cfg);  // the rest of its characters, should any be drawn
    // a dark outline in the sheet (opaque, dark pixels): the atlas's letters get one of their own (FillSheetFont)
    size_t dark = 0;
    for (size_t i = 0; i < size_t(sf.w) * sf.h; i++) {
        const uint8_t* p = &sf.px[i * 4];
        dark += p[3] > 160 && std::max({p[0], p[1], p[2]}) < 64;
    }
    sf.outline = dark > size_t(sf.w) * sf.h / 200;
    for (int gi = 0; gi < mf.count; gi++) {
        const MenuGlyph& g = mf.glyphs[gi];
        // Each letter's rectangle of the sheet and a texel more round it (room for its outline), then two
        // transparent pixels round that. The glyph is drawn from all but the outer one of those (FillSheetFont pulls
        // it in after the build): ImGui packs the next rectangle right against this one, and a glyph drawn to the
        // rectangle's very edge took in a little of it through the filtering, a faint line under or beside the
        // letter wherever it landed between pixels.
        int gw = int(std::lround((g.x1 - g.x0 + 3) * k)) + 4, gh = int(std::lround((g.y1 - g.y0 + 3) * k)) + 4;
        sf.ids.push_back(io.Fonts->AddCustomRectFontGlyph(sf.im, ImWchar(static_cast<unsigned char>(g.c)), gw, gh,
                                                          g.advance * k, ImVec2(-k - 2, (g.dy + 1) * k - 2)));
    }
    sf.space = io.Fonts->AddCustomRectFontGlyph(sf.im, ImWchar(' '), 1, 1, mf.space * k, ImVec2(0, 0));
    return true;
}

// The sheet at its own size (a texel a pixel), its letters as connected shapes: a glyph keeps only the pixels of
// its own shapes (those mostly inside its rectangle) and a texel round them, so the upscaled image's blur of the
// letters next to it in the sheet does not show as lines along its edges.
struct SheetShapes {
    int w = 256, h = 128;
    std::vector<int> label;  // per texel: shape number + 1, 0 none
    std::vector<int> size;   // per shape: texels
};

SheetShapes Shapes(const SheetFont& sf) {
    SheetShapes sh;
    sh.h = int(std::lround(sf.h / sf.k));
    int w = sh.w, h = sh.h, k = std::max(1, int(std::lround(sf.k)));
    std::vector<uint8_t> on(size_t(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int dy = 0; dy < k; dy++)
                for (int dx = 0; dx < k; dx++) {
                    int sx = std::min(int(sf.w) - 1, x * k + dx), sy = std::min(int(sf.h) - 1, y * k + dy);
                    sum += WhiteAt(sf, size_t(sy) * sf.w + sx);
                }
            on[size_t(y) * w + x] = sum / (k * k) > 40;
        }
    sh.label.assign(on.size(), 0);
    std::vector<int> stack;
    for (int i = 0; i < w * h; i++) {
        if (!on[size_t(i)] || sh.label[size_t(i)]) continue;
        int n = int(sh.size.size()) + 1, count = 0;
        stack.push_back(i);
        sh.label[size_t(i)] = n;
        while (!stack.empty()) {
            int p = stack.back();
            stack.pop_back();
            count++;
            int px = p % w, py = p / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int qx = px + dx, qy = py + dy;
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    int q = qy * w + qx;
                    if (on[size_t(q)] && !sh.label[size_t(q)]) sh.label[size_t(q)] = n, stack.push_back(q);
                }
        }
        sh.size.push_back(count);
    }
    return sh;
}

// the texels a glyph keeps: its shapes and one texel round them that no other shape has (over its rectangle and a
// texel round it)
std::vector<uint8_t> GlyphMask(const SheetShapes& sh, const MenuGlyph& g) {
    int x0 = g.x0, x1 = g.x1, y0 = g.y0, y1 = g.y1;
    std::vector<int> inside(sh.size.size() + 1, 0);
    for (int y = y0; y < y1 && y < sh.h; y++)
        for (int x = x0; x < x1 && x < sh.w; x++) inside[size_t(sh.label[size_t(y) * sh.w + x])]++;
    auto own = [&](int l) { return l > 0 && inside[size_t(l)] * 2 >= sh.size[size_t(l - 1)]; };
    int w = x1 - x0 + 3, h = y1 - y0 + 3;  // a texel of margin round the rectangle
    std::vector<uint8_t> m(size_t(w) * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = x0 - 1 + x, sy = y0 - 1 + y;
            if (sx < 0 || sy < 0 || sx >= sh.w || sy >= sh.h) continue;
            int l = sh.label[size_t(sy) * sh.w + sx];
            if (l && !own(l)) continue;
            bool keep = own(l);
            // outside the letter's own rectangle (the margin for its outline) only its own shapes: the faint edge of
            // the letter above or beside it in the sheet is not taken in
            bool in_rect = sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
            for (int dy = -1; dy <= 1 && !keep && in_rect; dy++)
                for (int dx = -1; dx <= 1 && !keep; dx++) {
                    if (dx != 0 && dy != 0) continue;
                    int qx = sx + dx, qy = sy + dy;
                    if (qx >= 0 && qy >= 0 && qx < sh.w && qy < sh.h) keep = own(sh.label[size_t(qy) * sh.w + qx]);
                }
            m[size_t(y) * w + x] = keep;
        }
    return m;
}

// after the atlas is built: the glyphs' pixels from the sheet, white with the sheet's coverage (and a dark outline
// round them when the sheet has one)
void FillSheetFont(ImGuiIO& io, const SheetFont& sf) {
    unsigned char* atlas = nullptr;
    int aw = 0, ah = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    SheetShapes shapes = Shapes(sf);
    constexpr int kBorder = 2;  // transparent pixels round each rectangle (AddSheetFont)
    auto put = [&](const ImFontAtlasCustomRect* r, const MenuGlyph* g) {
        int W = r->Width, H = r->Height;
        std::vector<float> cov(size_t(W) * H, 0.0f);  // the letter's coverage, 0-1
        if (g) {
            std::vector<uint8_t> mask = GlyphMask(shapes, *g);
            int mw = g->x1 - g->x0 + 3, mh = g->y1 - g->y0 + 3;
            for (int y = kBorder; y < H - kBorder; y++)
                for (int x = kBorder; x < W - kBorder; x++) {
                    // the sheet's pixels under the glyph's rectangle and a texel round it, one for one (a resampling
                    // here made the edges jagged)
                    int ix = int(std::lround((g->x0 - 1) * sf.k)) + x - kBorder, iy = int(std::lround((g->y0 - 1) * sf.k)) + y - kBorder;
                    if (ix < 0 || iy < 0 || ix >= int(sf.w) || iy >= int(sf.h)) continue;
                    // the mask between its texels' centres, so its edge is a slope and not steps
                    float fx = std::clamp((ix + 0.5f) / sf.k - 0.5f - float(g->x0 - 1), 0.0f, float(mw - 1));
                    float fy = std::clamp((iy + 0.5f) / sf.k - 0.5f - float(g->y0 - 1), 0.0f, float(mh - 1));
                    int jx = int(fx), jy = int(fy), jx1 = std::min(jx + 1, mw - 1), jy1 = std::min(jy + 1, mh - 1);
                    float ax = fx - jx, ay = fy - jy;
                    auto m = [&](int mx, int my) { return mask[size_t(my) * mw + mx] ? 1.0f : 0.0f; };
                    float mv = (m(jx, jy) * (1 - ax) + m(jx1, jy) * ax) * (1 - ay) + (m(jx, jy1) * (1 - ax) + m(jx1, jy1) * ax) * ay;
                    // in the margin round the rectangle, a texel not kept is not taken at all (its pixels may be the
                    // next letter's)
                    int tx = int(ix / sf.k), ty = int(iy / sf.k);
                    if ((tx < g->x0 || tx > g->x1 || ty < g->y0 || ty > g->y1) && !m(tx - (g->x0 - 1), ty - (g->y0 - 1))) mv = 0;
                    float c = float(WhiteAt(sf, size_t(iy) * sf.w + ix)) * std::min(1.0f, mv * 1.5f);
                    // the upscaled sheet's faint haze (its blur of neighbouring letters) goes, the edges stay smooth
                    cov[size_t(y) * W + x] = std::clamp((c - 20.0f) / 235.0f, 0.0f, 1.0f);
                }
        }
        // the outline: the letter's own coverage spread a little over half a texel, dark (made here from the letter
        // alone, so no part of a neighbour's outline comes with it)
        std::vector<float> ring(cov.size(), 0.0f);
        if (g && sf.outline) {
            float rr = 0.55f * sf.k;
            int ri = int(std::ceil(rr + 0.5f));
            for (int y = kBorder; y < H - kBorder; y++)
                for (int x = kBorder; x < W - kBorder; x++) {
                    float best = 0;
                    for (int dy = -ri; dy <= ri; dy++)
                        for (int dx = -ri; dx <= ri; dx++) {
                            int qx = x + dx, qy = y + dy;
                            if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
                            float c = cov[size_t(qy) * W + qx];
                            if (c <= best) continue;
                            float reach = std::clamp(rr + 0.5f - std::sqrt(float(dx * dx + dy * dy)), 0.0f, 1.0f);
                            best = std::max(best, c * reach);
                        }
                    ring[size_t(y) * W + x] = best * 0.85f;
                }
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float c = cov[size_t(y) * W + x], o = ring[size_t(y) * W + x];
                float a = c + o * (1 - c);
                unsigned char* d = atlas + (size_t(r->Y + y) * aw + r->X + x) * 4;
                // straight alpha: white over the dark outline (an outlined sheet's clear pixels dark too, so the
                // filtering does not lighten its edge)
                uint8_t v = a > 0.0f ? uint8_t(std::clamp(c / a * 255.0f + 0.5f, 0.0f, 255.0f)) : (sf.outline ? 0 : 255);
                d[0] = d[1] = d[2] = v;
                d[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
            }
    };
    for (size_t i = 0; i < sf.ids.size(); i++) put(io.Fonts->GetCustomRectByIndex(sf.ids[i]), &sf.font->glyphs[i]);
    put(io.Fonts->GetCustomRectByIndex(sf.space), nullptr);
    // each glyph drawn from all but its rectangle's outer pixel (AddSheetFont): the filtering stays inside it
    float pu = 1.0f / float(aw), pv = 1.0f / float(ah);
    for (int gi = 0; gi < sf.font->count; gi++) {
        auto* glyph = const_cast<ImFontGlyph*>(sf.im->FindGlyphNoFallback(ImWchar(static_cast<unsigned char>(sf.font->glyphs[gi].c))));
        if (!glyph) continue;
        glyph->X0 += 1, glyph->Y0 += 1, glyph->X1 -= 1, glyph->Y1 -= 1;
        glyph->U0 += pu, glyph->V0 += pv, glyph->U1 -= pu, glyph->V1 -= pv;
    }
    // the space: nothing drawn (its 1x1 picture, drawn, took in its neighbours through the filtering)
    if (auto* sp = const_cast<ImFontGlyph*>(sf.im->FindGlyphNoFallback(ImWchar(' ')))) sp->Visible = 0;
    LOGI("menus: the pages use the game's lettering %s (%ux%u sheet%s)", sf.font->key, sf.w, sf.h, sf.outline ? ", outlined" : "");
}

// The keyboard's keys for the button prompts: pictures in the texture pack's folders (kb_enter.png and so on), put
// into ImGui's atlas beside the lettering.
struct KeyPicture {
    const wchar_t* file;
    std::vector<uint8_t> px;  // BGRA
    UINT w = 0, h = 0;
    int id = -1;
    float aspect = 1;
    ImVec2 uv0, uv1;
    bool ready = false;
};
KeyPicture g_keys[4] = {{L"kb_enter.png"}, {L"kb_backspace.png"}, {L"kb_updown.png"}, {L"kb_leftright.png"}};  // Prompt order

std::filesystem::path FindFile(const wchar_t* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = app::ExeDir() / "textures" / "replacement";
    if (!fs::is_directory(root, ec)) return {};
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::wstring name = it->path().filename().wstring();
        for (auto& ch : name) ch = wchar_t(towlower(ch));
        if (name == file) return it->path();
    }
    return {};
}

// any picture as BGRA (LoadSheet wants a font sheet's proportions)
bool LoadPicture(const std::filesystem::path& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* src = nullptr;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &src);
    if (SUCCEEDED(hr)) hr = src->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w < 8 || h < 8 || w > 2048 || h > 2048)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        bgra.resize(size_t(w) * h * 4);
        hr = src->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data());
    }
    if (src) src->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    wic->Release();
    return SUCCEEDED(hr);
}

constexpr int kKeyHeight = 44;  // atlas pixels a key picture is tall (a little soft, like the game's own pictures)

void AddKeyPictures(ImGuiIO& io) {
    for (KeyPicture& k : g_keys) {
        std::filesystem::path p = FindFile(k.file);
        if (p.empty() || !LoadPicture(p, k.px, k.w, k.h)) continue;
        k.aspect = float(k.w) / float(k.h);
        // a transparent pixel round it, as the letters (FillKeyPictures)
        k.id = io.Fonts->AddCustomRectRegular(std::max(1, int(std::lround(kKeyHeight * k.aspect))) + 2, kKeyHeight + 2);
    }
}

void FillKeyPictures(ImGuiIO& io) {
    unsigned char* atlas = nullptr;
    int aw = 0, ah = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    for (KeyPicture& k : g_keys) {
        if (k.id < 0) continue;
        ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(k.id);
        // box filtered down to the rectangle inside its transparent border
        int iw = r->Width - 2, ih = r->Height - 2;
        for (int y = 0; y < r->Height; y++)
            for (int x = 0; x < r->Width; x++) {
                if (x == 0 || y == 0 || x == r->Width - 1 || y == r->Height - 1) {
                    unsigned char* d = atlas + (size_t(r->Y + y) * aw + r->X + x) * 4;
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
                int ix = x - 1, iy = y - 1;
                int sx0 = int(float(ix) * k.w / iw), sx1 = std::max(sx0 + 1, int(float(ix + 1) * k.w / iw));
                int sy0 = int(float(iy) * k.h / ih), sy1 = std::max(sy0 + 1, int(float(iy + 1) * k.h / ih));
                float acc[4] = {}, n = 0;
                for (int sy = sy0; sy < sy1 && sy < int(k.h); sy++)
                    for (int sx = sx0; sx < sx1 && sx < int(k.w); sx++) {
                        const uint8_t* q = &k.px[(size_t(sy) * k.w + sx) * 4];
                        float a = q[3] / 255.0f;
                        acc[0] += q[2] * a, acc[1] += q[1] * a, acc[2] += q[0] * a, acc[3] += q[3];
                        n++;
                    }
                unsigned char* d = atlas + (size_t(r->Y + y) * aw + r->X + x) * 4;
                float alpha = n ? acc[3] / n : 0;
                for (int c = 0; c < 3; c++) d[c] = uint8_t(alpha > 0 ? std::clamp(acc[c] / n / (alpha / 255.0f), 0.0f, 255.0f) : 0);
                d[3] = uint8_t(std::clamp(alpha, 0.0f, 255.0f));
            }
        io.Fonts->CalcCustomRectUV(r, &k.uv0, &k.uv1);
        ImVec2 px((k.uv1.x - k.uv0.x) / r->Width, (k.uv1.y - k.uv0.y) / r->Height);
        k.uv0.x += px.x, k.uv0.y += px.y, k.uv1.x -= px.x, k.uv1.y -= px.y;
        k.ready = true;
        k.px.clear();
    }
}

// the keyboard's keys over the prompts the game drew this frame
void DrawPrompts() {
    if (!Keyboard()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    for (const PromptRect& p : ShownPrompts()) {
        const KeyPicture& key = g_keys[int(p.prompt)];
        if (!key.ready) continue;
        float cx = (p.x0 + p.x1) / 2, cy = (p.y0 + p.y1) / 2;
        // a button the game gives no words: no key either; else the key on the words' middle line
        float mid = cy;
        bool labelled = Labelled(p, &mid);
        if (kDrivingImage && !labelled) continue;
        if (labelled) cy = mid;
        // the main menus' pictures are bigger than the pause menus': their keys a little smaller to match
        float k = kDrivingImage || Paused() ? 1.0f : 0.88f;
        bool arrows = p.prompt == Prompt::UpDown || p.prompt == Prompt::LeftRight;
        float h, w;
        if (arrows) {
            // the size they had, then a sixth smaller; the right end stays where it was, the same gap from the words
            auto size = [&](float f, float& hh, float& ww) {
                hh = (p.y1 - p.y0) * f * k, ww = hh * key.aspect;
                float most = (p.x1 - p.x0) * 1.3f * k * f / 0.7f;
                if (ww > most) ww = most, hh = ww / key.aspect;
            };
            float h0, w0;
            size(0.7f, h0, w0);
            size(0.58f, h, w);
            cx += (w0 - w) / 2;
        } else {
            // ENTER and BACKSPACE: as wide as the button, its right end at the button's, close to the words
            h = (p.y1 - p.y0) * 0.56f * k, w = h * key.aspect;
            float most = (p.x1 - p.x0) * 0.95f * k;
            if (w > most) w = most, h = w / key.aspect;
            cx = p.x1 - w / 2 + (p.x1 - p.x0) * 0.08f;
        }
        int alpha = int(std::clamp(p.alpha, 0.0f, 1.0f) * 255 + 0.5f);
        if (alpha <= 0) continue;
        dl->AddImage(tex, Guest(cx - w / 2, cy - h / 2), Guest(cx + w / 2, cy + h / 2), key.uv0, key.uv1,
                     IM_COL32(255, 255, 255, alpha));
    }
}

void Window(void*) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();  // the F10 panel's
    wchar_t dir[MAX_PATH] = {};
    GetWindowsDirectoryW(dir, MAX_PATH);
    for (const wchar_t* name : {L"\\Fonts\\bahnschrift.ttf", L"\\Fonts\\segoeuib.ttf", L"\\Fonts\\arialbd.ttf"}) {
        std::wstring path = std::wstring(dir) + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        g_font = io.Fonts->AddFontFromFileTTF(utf8, 48.0f, &cfg);
        if (g_font) break;
    }
    if (kMenus || kDrivingImage) {
        // the body in the options list's lettering, titles in the italic one
        static SheetFont body, title;  // kept until the atlas has their pixels
        bool have_body = AddSheetFont(io, kBodyFont, "AUF list", body);
        bool have_title = AddSheetFont(io, kTitleFont, "AUF titles", title);
        AddKeyPictures(io);
        bool have_keys = false;
        for (const KeyPicture& k : g_keys) have_keys |= k.id >= 0;
        if (have_body || have_title || have_keys) {
            io.Fonts->Build();
            FillKeyPictures(io);
            if (have_body) FillSheetFont(io, body), g_font = body.im, g_game_font = true;
            if (have_title) FillSheetFont(io, title), g_title_font = title.im;
            body.px.clear(), title.px.clear();
        }
    }
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (!(kMenus || kDrivingImage) || g_page.load() == Page::None) return false;
    if (PageMessage(msg, wp)) {
        *result = 0;
        return true;
    }
    if (msg != WM_KEYDOWN) return false;
    switch (wp) {
        case VK_UP: Push(Nav::Up); break;
        case VK_DOWN: Push(Nav::Down); break;
        case VK_LEFT: Push(Nav::Left); break;
        case VK_RIGHT: Push(Nav::Right); break;
        case VK_RETURN:
        case VK_SPACE: Push(Nav::Accept); break;
        case VK_ESCAPE:
            if (PausePage()) {
                g_escape_close.store(true);
                break;
            }
            Push(Nav::Back);
            break;
        case VK_BACK: Push(Nav::Back); break;
        case VK_PRIOR:
        case 'Q': Push(Nav::PrevTab); break;
        case VK_NEXT:
        case 'E': Push(Nav::NextTab); break;
        default: return false;
    }
    *result = 0;
    return true;
}

// the mouse in console coordinates, false off the frame
bool MouseGuest(float& x, float& y) {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMousePosValid()) return false;
    const float* r = app::g_frame_rect;
    float rx = r[0], ry = r[1], rw = r[2], rh = r[3];
    if (rw <= 0 || rh <= 0) rx = ry = 0, rw = io.DisplaySize.x, rh = io.DisplaySize.y;
    if (rw <= 0 || rh <= 0) return false;
    x = (io.MousePos.x - rx) * 640 / rw;
    y = (io.MousePos.y - ry) * 480 / rh;
    return x >= 0 && y >= 0 && x < 640 && y < 480;
}

// The mouse on the game's own menus (MouseSteer): the row under the cursor, selected when the cursor moves onto it,
// accepted when clicked. The rows' boxes in console pixels: the main menu's bars (QUIT GAME sixth), the options list's
// lines (PC Settings third), the pause menu's MISSION rows (Controls and PC Settings fourth and fifth).
void MouseMenus() {
    ImGuiIO& io = ImGui::GetIO();
    if (!Keyboard() || io.WantCaptureMouse || g_page.load() != Page::None) return;
    MouseMenu menu = MouseMenu::None;
    int rows = 0;
    float x0 = 0, x1 = 0, h = 0;
    auto top = [&](int r) -> float {
        switch (menu) {
            case MouseMenu::Main: return kNewTop + kNewStep * float(r);
            case MouseMenu::Options:
                if (r <= kControllerRow) return kRowTop + kRowStep * float(r);
                return kRowTop + kRowStep * float(r - 1) + kInsert;  // PC Settings, then the rows moved down under it
            case MouseMenu::Pause:
                return kDrivingImage ? kDriveRowsTop + kDriveStep * float(r) : kPauseTop - kPauseLift + kPauseStep * float(r);
            default: return -1000;
        }
    };
    if (Paused()) {
        menu = MouseMenu::Pause, rows = kPauseRows + kPauseExtraRows, x0 = 95, x1 = 430, h = kPauseStep;
    } else if (kMenus && Recent(g_main_seen.load())) {
        menu = MouseMenu::Main, rows = kMainItems + 1, x0 = kBarX0, x1 = kBarX1, h = kNewStep - 6;
    } else if (kMenus && Recent(g_options_seen.load())) {
        menu = MouseMenu::Options, rows = kOptionRows + 1, x0 = kLabelX - 8, x1 = 600, h = kRowStep;
    }
    int cur = MouseRowNow(menu);
    float mx, my;
    if (cur < 0 || !MouseGuest(mx, my)) return;
    int hover = -1;
    for (int r = 0; r < rows; r++) {
        float y = top(r);
        if (mx >= x0 && mx <= x1 && my >= y - 3 && my < y - 3 + h) hover = r;
    }
    if (hover < 0 || hover == cur) return;
    bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    bool click = ImGui::IsMouseClicked(0);
    if (!moved && !click) return;
    g_mouse_menu.store(int(menu));
    g_mouse_at.store(NowMs());
    g_mouse_target.store(hover);
    // a click on a row not yet selected: its A waits for the selection to get there (a click on the selected row is
    // the menus' own A, controls.cpp)
    if (click) g_mouse_strip_a.store(true), g_mouse_accept.store(true);
}

// "Press ENTER" where the title screen's "Press START" was, in the same lettering, size and colour
void PressEnter() {
    if (!Keyboard() || !InShownFrame(g_press_seen.load()) || !g_font) return;
    PressLine p;
    {
        std::lock_guard lock(g_prompt_lock);
        p = g_press;
    }
    const MenuGlyph* cap = FindGlyph(kBodyFont, 'P');
    if (!cap || p.first_h <= 0) return;
    // console pixels a texel of the sheet: the line's P against the sheet's
    float k = p.first_h / float(cap->y1 - cap->y0 + 1);
    float size = kBodyFont.line_height * k * GuestScale();
    const char* text = "Press ENTER";
    ImVec2 extent = g_font->CalcTextSizeA(size, FLT_MAX, 0, text);
    // the font's cell puts a capital's top (dy + 2) texels below the text's top (AddSheetFont)
    ImVec2 at = Guest((p.x0 + p.x1) / 2, p.y0 - float(cap->dy + 2) * k);
    at.x -= extent.x / 2;
    ImU32 col = IM_COL32(int(std::clamp(p.color[0], 0.0f, 1.0f) * 255), int(std::clamp(p.color[1], 0.0f, 1.0f) * 255),
                         int(std::clamp(p.color[2], 0.0f, 1.0f) * 255), int(std::clamp(p.color[3], 0.0f, 1.0f) * 255));
    ImGui::GetBackgroundDrawList()->AddText(g_font, size, at, col, text);
}

void Ui() {
    if (kMenus) PressEnter();
    if (kMenus || kDrivingImage) DrawPrompts();
    if (!kMenus && !kDrivingImage) return;
    MouseMenus();
    Page page = g_page.load();
    if (page == Page::None) {
        // exactly on the frames the game draws its Controller Setup: not on the black screen after it
        if (kMenus && InShownFrame(g_setup_seen.load())) {
            controls::Context c = g_mission_query ? g_mission_query() : controls::Context::OnFoot;
            ControlsList(Context(), c, 130, 14);
        }
        return;
    }
    std::vector<Nav> nav = TakeNav();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    // the pages take the mouse, unless the controller is in use (the cursor is hidden then, controls.cpp)
    if (controls::ActiveDevice() == controls::Device::KeyboardMouse) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (page == Page::Quit) {
        QuitPage(nav);
        return;
    }
    bool opened = g_opened.exchange(false);
    if (page == Page::PauseControls || page == Page::PausePcSettings) {
        // on the pause menu's own panel (its moving background, frame and prompts' bar), whose rows and words are
        // hidden meanwhile (Draw, DrivingPauseDraw)
        std::vector<PageNav> pn;
        for (Nav n : nav) pn.push_back(PageNav(int(n)));
        bool open = page == Page::PauseControls ? ControlsViewPage(Context(), pn, controls::CurrentContext(), opened)
                                                : PausePcSettingsPage(Context(), pn, controls::CurrentContext(), opened);
        if (!open) g_page.store(Page::None);
        return;
    }
    std::vector<PageNav> pn;
    for (Nav n : nav) pn.push_back(PageNav(int(n)));
    bool open = page == Page::PcSettings ? PcSettingsPage(Context(), pn) : ControlsPage(Context(), pn);
    if (!open) g_page.store(Page::None);
}

}  // namespace menus
#endif
