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
// any_projection: also under a level's projection (the positions are screen positions all the same), as a match's own
// screens are drawn
Layout MenuLayout(const gpu::State& s, uint32_t prim, uint32_t stride, uint32_t count, bool any_projection = false) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Layout l;
    if (prim != kTriangleList || count < 6 || count % 6 || !s.vs || !s.vs->program.empty()) return l;
    if (!any_projection && std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return l;
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

// each edge of the part of a size x size texture a quad draws half a texel in, so the filtering stays inside it; done the
// same way however often the game draws the quad
void InsetUV(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, float size) {
    for (int c = 0; c < 2; c++) {
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t i = 0; i < count; i++) {
            float x = At(v, stride, i, l.tc)[c] * size;
            lo = std::min(lo, x), hi = std::max(hi, x);
        }
        if (hi - lo < 4) continue;
        float base_lo = std::floor(lo + 0.25f), base_hi = std::ceil(hi - 0.25f);
        for (uint32_t i = 0; i < count; i++) {
            float* q = At(v, stride, i, l.tc);
            float x = q[c] * size;
            q[c] = (x - lo < hi - x ? base_lo + 0.5f : base_hi - 0.5f) / size;
        }
    }
}

// The on-foot pause menu's >> marker: the game draws texels 54 to 63 across and 28 to 44 down of the 64x64 sheet
// (4135a1a1, its pack key) mirrored on 11 x 17 console pixels (x 87 to 98, the tips on the right at u 54), which cuts
// the sheet's << at its edges and leaves no room round it for an outline (the A button's green left of texel 54, the
// sheet's edge right of 63). With the texture pack's menu part on, and its picture of the sheet holding the marker in
// the free part kMarker (texels 19 to 29 across, 46 to 64 down: AddKeyPictures looks), the quad draws that part
// instead, a quarter texel in from its edges (no neighbour taken in by the filtering at the pack's 4x), on 14 x 17
// console pixels: its tips' end where the game's was (x 98), its back 3 pixels further left (84), the same rows. With
// the menu part off the game's own marker as it was.
constexpr float kMarkerU0 = 19.25f, kMarkerU1 = 28.75f, kMarkerV0 = 46.25f, kMarkerV1 = 63.75f, kMarkerWide = 14;
std::atomic<bool> g_marker_in_pack{false};
// true: the quad draws the pack's marker (not to be taken half a texel in, InsetUV)
bool MoveMarker(uint8_t* v, uint32_t stride, const Layout& l) {
    if (!g_marker_in_pack.load() || !overlay::g_hd_menus || !app::Get().custom_textures) return false;
    float lo = 1e9f, hi = -1e9f, x_lo = 0, x_hi = 0;
    for (uint32_t i = 0; i < 6; i++) {
        float u = At(v, stride, i, l.tc)[0] * 64, x = At(v, stride, i, l.pos)[0];
        if (u < lo) lo = u, x_lo = x;
        if (u > hi) hi = u, x_hi = x;
    }
    if (Near(lo, kMarkerU0, 0.05f)) return true;  // done already (the game drew the same vertices again)
    if (!Near(lo, 54, 0.6f) || !Near(hi, 63, 0.6f) || x_lo == x_hi) return false;
    float vlo = 1e9f;
    for (uint32_t i = 0; i < 6; i++) vlo = std::min(vlo, At(v, stride, i, l.tc)[1] * 64);
    const float dir = x_hi > x_lo ? 1.0f : -1.0f;
    for (uint32_t i = 0; i < 6; i++) {
        float* t = At(v, stride, i, l.tc);
        float* p = At(v, stride, i, l.pos);
        bool tips = t[0] * 64 - lo < hi - t[0] * 64;
        p[0] = tips ? x_lo : x_lo + dir * kMarkerWide;
        t[0] = (tips ? kMarkerU0 : kMarkerU1) / 64;
        t[1] = (Near(t[1] * 64, vlo, 1) ? kMarkerV0 : kMarkerV1) / 64;
    }
    return true;
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
// the first frame the game drew with a pause page's treatment of its panel (no words, no running figure, taller), plus
// one; 0 not yet. The page is drawn over the frames from that one on: a frame drawn before the page opened (still on
// its way to the screen) shows the pause menu as it was, the figure too, and the page over it showed the figure for a
// moment.
std::atomic<uint64_t> g_page_from{0};
void PageTreated() {
    if (!g_page_from.load()) g_page_from.store(Frame() + 1);
}

// The pause Controls page's controller layout: its tabs (Controller, Keyboard & Mouse) are drawn in the tabs' bar over
// the game's MISSION tab, whose light box stayed behind Controller whichever tab was selected. While the layout is up,
// the page reports the selected tab's span (Ui, console pixels) and the game's light box is drawn there instead, the
// bar's dark pieces round it (Draw / AfterDraw on foot, DrivingPanelPiece in driving.xbe).
std::atomic<bool> g_layout_tab_on{false};
std::atomic<float> g_layout_tab_x0{0}, g_layout_tab_x1{0};
bool LayoutTab(float& x0, float& x1) {
    if (!g_layout_tab_on.load() || !PausePage()) return false;
    x0 = g_layout_tab_x0.load(), x1 = g_layout_tab_x1.load();
    return x1 > x0;
}

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
// For the draws: paused, or paused earlier in this frame or in the frame before. The game clears its pause flag in the
// frame in which it shuts the menu, and draws the whole pause menu once more in that frame: treated as not paused it came out
// as the game's own layout (the rows 20 lower, no Controls / PC Settings) for that one frame, a flicker on the way out.
std::atomic<uint64_t> g_paused_drawn{0};  // the last frame drawn while paused, plus one
bool DrawPaused() {
    if (Paused()) {
        g_paused_drawn.store(Frame() + 1);
        return true;
    }
    // (the flag is cleared part way through that frame's draws: paused for some of them, or in the frame before)
    uint64_t p = g_paused_drawn.load();
    return p && p + 1 >= Frame() + 1 && p <= Frame() + 1;
}

// The five rows sat low on the panel, PC Settings against the Lives Remaining / Mission Duration bar: the game's rows,
// the added ones and the marker all go this far up, the block in the middle of the panel.
constexpr float kPauseLift = 20;

// a row of the pause menu's MISSION page, -1 when not one; *lifted: already moved up by kPauseLift (a line the game
// keeps and draws again is moved once only). The two grids are 6 apart, each row is matched to 3.
int PauseRow(const gpu::State& s, const Box& b, bool* lifted = nullptr, float x = kPauseX) {
    if (!TextureSize(s, 256, 128) || !Near(b.x0, x, 3)) return -1;
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

// The "Are you sure?" question (Restart or Quit Mission): the game draws it in place of that row, its Yes and No on
// the same line further right (x 293 and 352), on a band of untextured quads (x 70 to 471, its top line 6 above the
// row's letters, the dark band from 5 above, its bottom line 19 below). The >> marker is not drawn meanwhile, and the
// game's own menu code keeps the pad to Yes / No. Its prompts' bar shows B BACK, which the MISSION page's own bar
// never has, and the bar is drawn before the rows: a frame of the pause menu that drew the B button before its rows is
// one with the question up (g_question_seen, the frame plus one).
std::atomic<uint64_t> g_question_seen{0};
// the row the question stands in (1 Restart, 2 Quit Mission), from its band. With the question up only Continue
// Mission and the question are shown, not the other of Restart and Quit Mission: a game row past Continue that is
// drawn before the band in that frame (Quit Mission, under Restart's question), or that is not the band's row (Restart
// Mission, drawn after Quit's band) is that other row; the question is drawn after its band, on its row. Packed with
// the frame it was seen in: (frame + 1) * 4 + row.
std::atomic<uint64_t> g_question_row{0};
int QuestionRow() {
    uint64_t q = g_question_row.load();
    return q && q / 4 == Frame() + 1 ? int(q % 4) : -1;
}
// the question up in the frame being drawn (the draws), or in this or the last frame (the pad, between frames)
bool QuestionDrawn() { return DrawPaused() && g_question_seen.load() == Frame() + 1; }
bool PauseQuestion() {
    uint64_t seen = g_question_seen.load();
    return Paused() && seen && Frame() + 1 - seen <= 1;
}

// the line's top on the rows' grid (as the game draws it, or already moved up): the row, -1 off it
int OnRowGrid(float y0, float offset, bool* lifted) {
    for (int up = 0; up < 2; up++) {
        float r = (y0 - offset - (kPauseTop - (up ? kPauseLift : 0))) / kPauseStep;
        int row = int(std::lround(r));
        if (row < 0 || row >= kPauseRows || std::fabs(r - row) * kPauseStep > 0.6f) continue;
        if (lifted) *lifted = up != 0;
        return row;
    }
    return -1;
}

// the question's answers for the mouse (MouseMenus): a quad of answer i (0 the left, 1 the right) drawn, and how
// bright; driving.xbe: a quad of the answer being drawn now (DrivingQuestionWord)
void AnswerQuad(int i, float x0, float y0, float x1, float y1, float bright);
void AnswerLetter(float x0, float y0, float x1, float y1);
// the mission select's panel on side i (0 left, 1 right) drawn there
void SidePanel(int i, float x0, float y0, float x1, float y1);

// a piece of the question's band (untextured: the position only), moved up with the rows once
bool QuestionBand(const gpu::State& s, uint8_t* v, uint32_t stride, uint32_t count) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (count != 6 || s.textures[0] || !s.vs || !s.vs->program.empty()) return false;
    if (std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return false;
    const auto& pos = s.vs->inputs[0];
    if (pos.stream != 0 || pos.type != kFloat4 || pos.offset + 16 > stride) return false;
    Layout l;
    l.pos = pos.offset;
    Box b = BoxOf(v, stride, l, 0, count);
    if (!(Near(b.x0, 70, 2) || Near(b.x0, 270, 2)) || !Near(b.x1 - b.x0, 201, 2) || b.y1 - b.y0 > 26) return false;
    for (float offset : {-6.0f, -5.0f, 19.0f}) {
        bool lifted = false;
        int row = OnRowGrid(b.y0, offset, &lifted);
        if (row < 0) continue;
        g_question_row.store((Frame() + 1) * 4 + uint64_t(row));
        if (!lifted) MoveY(v, stride, l, count, -kPauseLift);
        return true;
    }
    return false;
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
// how much of the setup page shows in each of the last few frames (its title's own colour, times what was drawn over
// it in that frame: a loading picture, a fade): the controls list is drawn only while the page itself shows
struct FrameLevel {
    uint64_t frame = ~0ull;
    float level = 0;
};
std::array<FrameLevel, 4> g_setup_level;
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

bool SetupTitle(const gpu::State& s, const Box& b, uint32_t count, const uint8_t* v, uint32_t stride, const Layout& l) {
    // the letters' box starts at 90,76 and the shadow's a pixel down and right (less once tucked in): either counts
    if (!(count == 96 && TextureSize(s, 256, 128) && b.x0 > 87 && b.x0 < 94 && b.y0 > 73 && b.y0 < 81)) return false;
    // and its letters are CONTROLLER SETUP: other titles at that place type themselves out a letter at a time, and
    // were taken for it at their 16th (MULTIPLAYER GAMES..., Combat Training Se...)
    constexpr std::string_view kText = "CONTROLLER SETUP";
    for (uint32_t q = 0; q < 16; q++) {
        const MenuGlyph* g = FindGlyph(kTitleFont, kText[q]);
        if (!g) continue;
        Box gb = BoxOf(v, stride, l, q * 6, 6);
        if (!Near(gb.u0 * 256.0f, g->x0) || !Near(gb.v0 * 128.0f, g->y0)) return false;
    }
    return true;
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

// bond_eng.xbe's tabs' bar (untextured quads, a float4 colour after the position): the lit tab's light box (1), the
// dark rest of the bar after it (2), else 0. On the game's own panel (y 111 to 131) or a page's stretched one.
constexpr float kFootTabsLeft = 95;  // where the bar's first box starts
// (any: the light box and the dark pieces wherever they are on the bar)
int FootTabPiece(const gpu::State& s, const uint8_t* v, uint32_t stride, uint32_t count, Layout& l, bool any = false) {
    if (count != 6 || s.textures[0] || !s.vs || !s.vs->program.empty()) return 0;
    int color = -1;
    for (int r = 1; r < 16; r++) {
        const auto& in = s.vs->inputs[size_t(r)];
        if (r != 9 && in.stream == 0 && in.type == kFloat4 && in.offset + 16 <= stride) color = int(in.offset);
    }
    if (color < 0) return 0;
    l.color = uint32_t(color);
    Box b = BoxOf(v, stride, l, 0, count);
    if (b.y0 < 70 || b.y0 > 115 || b.y1 < 100 || b.y1 > 135 || b.y1 - b.y0 < 15 || b.x1 < 200) return 0;
    auto c = ColorOf(v, stride, l);
    auto is = [&](float r, float g, float bl) { return Near(c[0], r, 0.06f) && Near(c[1], g, 0.06f) && Near(c[2], bl, 0.06f); };
    if (is(0.565f, 0.796f, 0.961f) && (any || b.x0 < 120)) return 1;
    if (is(0.082f, 0.086f, 0.251f) && (any || b.x1 > 630)) return 2;
    return 0;
}

// a quad's left side (its vertices left of its middle) to x0, its right side to x1
void SpanX(uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, float x0, float x1) {
    Box b = BoxOf(v, stride, l, 0, count);
    for (uint32_t i = 0; i < count; i++) {
        float& x = At(v, stride, i, l.pos)[0];
        x = x < (b.x0 + b.x1) / 2 ? x0 : x1;
    }
}

// the tabs' bar's light box as last drawn, and the frame (plus one) it was drawn in
std::atomic<float> g_tab_box_x0{0}, g_tab_box_x1{0};
std::atomic<uint64_t> g_tab_box_seen{0};

// bond_eng.xbe's pause menu, its tabs' bar (on a page after its stretch and the layout tab's box): the dark pieces
// either side of the lit tab's light box end where the box starts and start where it ends. The box is see-through,
// and the dark piece left of OBJECTIVES' box reached 16 pixels into it (a darker strip at the box's left edge), the
// others a pixel. The box is drawn after the piece left of it: its place in the frame before counts for that one, when
// it starts near that piece's end (a tab just selected keeps the game's piece for a frame).
void FootTabBar(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    if (prim != kTriangleList || count != 6 || s.textures[0] || !s.vs || !s.vs->program.empty()) return;
    const auto& pos = s.vs->inputs[0];
    if (pos.stream != 0 || pos.type != kFloat4 || pos.offset + 16 > stride) return;
    Layout l;
    l.pos = pos.offset;
    int piece = FootTabPiece(s, v, stride, count, l, true);
    if (!piece) return;
    Box b = BoxOf(v, stride, l, 0, count);
    if (piece == 1) {
        g_tab_box_x0.store(b.x0), g_tab_box_x1.store(b.x1);
        g_tab_box_seen.store(Frame() + 1);
        return;
    }
    if (!Recent(g_tab_box_seen.load())) return;
    float x0 = g_tab_box_x0.load(), x1 = g_tab_box_x1.load();
    if (b.x0 < x0 - 0.5f && b.x1 > x0 && b.x1 - x0 < 24) SpanX(v, stride, l, count, b.x0, x0);
    else if (b.x1 > x1 + 0.5f && b.x0 < x1 && x1 - b.x0 < 24) SpanX(v, stride, l, count, x1, b.x1);
}

bool PausePanelDraw(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    PageTreated();
    if (prim != kTriangleList || count < 6 || count % 6 || !s.vs || !s.vs->program.empty()) return false;
    if (std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return false;
    const auto& pos = s.vs->inputs[0];
    if (pos.stream != 0 || pos.type != kFloat4 || pos.offset + 16 > stride) return false;
    Layout l;
    l.pos = pos.offset;
    Box b = BoxOf(v, stride, l, 0, count);
    // the running figure (a 128x128 picture right of the rows)
    // (anywhere right of the rows: it slides and grows in as the menu opens, so the first frames had it elsewhere)
    if (count == 6 && TextureSize(s, 128, 128) && b.x1 > 380 && b.y1 > 120) {
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
    bool marker = count == 6 && TextureSize(s, 64, 64) && Near(b.x0, kMarkerX, 3.5f);
    bool words = glyphs && s.textures[0] && TextureSize(s, 256, 128) && b.y0 < kFootWordsEnd;
    if (marker || words) {
        Hide(v, stride, l, count);
        return true;
    }
    // the Lives Remaining bar's top line and its dark box (its words gone): a light line across the page above the
    // prompts' bar, and a darker band under it; the moving background runs down to the prompts' bar instead
    if (count == 6 && !s.textures[0] && b.y0 > 286 && b.y1 < 316 && b.x0 >= 150 && b.x1 <= 570) {
        Hide(v, stride, l, count);
        return true;
    }
    // the controller layout's selected tab: the light box behind it, the dark rest from its right end (the part left of
    // it is added after this draw, AfterDraw)
    float t0, t1;
    Layout tl = l;
    if (int piece = FootTabPiece(s, v, stride, count, tl); piece && LayoutTab(t0, t1)) {
        if (piece == 1) SpanX(v, stride, l, count, std::max(t0, kFootTabsLeft), t1);
        else SpanX(v, stride, l, count, t1, b.x1);
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
bool PromptPicture(const gpu::State& s, float u0, float v0, float u1, float v1);
float PromptToWords(float x0, float y0, float x1, float y1, float picture_mid);

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

// driving.xbe's pause panel, as the game draws it (before a page's stretch), in all its pages:
// - the moving light-blue background (a 256x256 picture, its pack key 4a7ca93e, x 117 to 654, y 130 to 314) and the
//   dark layer drawn with it (the panel sheet's strip at 3,231) started at x 117, right of the frame's curved left
//   column (the panel sheet's left picture, x 21 to 117), and below the tab bar, where on foot it reaches the frame and
//   runs under the tab bar: its left side now runs from (60, 110) to (100, its bottom), inside the curve's inner line
//   all the way down (that line is at x 46 to 96 over those rows), and its top is at 110, under the tab bar;
// - the tab bar: the lit tab's light box ended a pixel short of the dark bar each side of it (a dark strip at its
//   edges, at PC resolutions several pixels), and the MAP tab's ended 20 short of the panel's right edge, where a dark
//   piece of the bar finished the row: the box goes a pixel further each side, a box that ends past x 600 to the edge,
//   and that last dark piece is not drawn;
// - on a page (Controls, PC Settings) the moving background a little stronger.
// x0..y1: the quad's box, kept up to date. True: the piece is not drawn.
bool DrivingPanelPiece(const gpu::State& s, uint8_t* v, uint32_t stride, bool page, float& x0, float& y0, float& x1,
                       float& y1) {
    uint8_t* col = StreamOf(s, v, 1);
    if (!col || s.streams[1].stride < 4) return false;
    uint32_t c0;
    std::memcpy(&c0, col, 4);
    const float w = x1 - x0, h = y1 - y0;
    // the tab bar's pieces (untextured, the bar's 22 rows): the lit box (0x5990CBF3 in mis02b, 0x5990CBF5 on rails) and
    // the dark pieces (0xDA181832, on rails 0xDA181841), their colours a little apart from mission to mission
    auto near = [](uint32_t c, uint32_t to) {
        for (int k = 0; k < 32; k += 8)
            if (std::abs(int((c >> k) & 0xFF) - int((to >> k) & 0xFF)) > 0x18) return false;
        return true;
    };
    if (!s.textures[0] && h > 18 && h < 26 && y0 > 100 && y0 < 120) {
        // the controller layout's selected tab (a pause page): the light box behind it, the dark pieces round it
        float t0 = 0, t1 = 0;
        const bool layout = page && LayoutTab(t0, t1);
        auto span = [&](float to0, float to1) {
            for (uint32_t i = 0; i < 4; i++) {
                float& x = At(v, stride, i, 0)[0];
                x = x < (x0 + x1) / 2 ? to0 : to1;
            }
            x0 = to0, x1 = to1;
        };
        if (near(c0, 0x5990CBF3u)) {
            // 2 past the game's ends: they met the dark pieces either side with a pixel to spare (a light strip at
            // PC resolutions); one that ends past x 600 to the panel's edge
            // (the first tab's box is not taken further left, to the curve: it is see-through, and over the curve's
            // dark picture it came out a different colour from the rest of it)
            float to0 = x0 - 2, to1 = x1 > 600 ? 640.0f : x1 + 2;
            if (layout) to0 = t0 < 100 ? to0 : t0, to1 = t1;  // the first tab keeps its left end
            span(to0, to1);
            g_tab_box_x0.store(to0), g_tab_box_x1.store(to1);
            g_tab_box_seen.store(Frame() + 1);
            return false;
        }
        if (near(c0, 0xDA181832u)) {
            // the one pixel of dark bar left of the first tab's box: inside the box now; with the layout's second
            // tab selected, the dark bar from there to that tab's box
            if (x0 < 90 && w < 3) {
                if (!layout || t0 < 100) return true;
                span(x0, t0);
                return false;
            }
            // the dark rest of the bar right of the box: from the layout tab's box on
            if (layout && x1 > 630 && x0 > 100) span(t1, x1);
            // and no piece reaching into the box (it is see-through: a darker strip along its edge), the box drawn
            // before them
            if (Recent(g_tab_box_seen.load())) {
                float b0 = g_tab_box_x0.load(), b1 = g_tab_box_x1.load();
                if (x0 < b0 - 0.5f && x1 > b0 && x1 - b0 < 24) span(x0, b0);
                else if (x1 > b1 + 0.5f && x0 < b1 && b1 - x0 < 24) span(b1, x1);
            }
            return x0 > 600 && w < 30;
        }
        return false;
    }
    // the moving background and its dark layer
    bool glow = TextureSize(s, 256, 256) && StageKey(s) == 0x4a7ca93e73c25de6ull;
    bool dark = false;
    if (!glow && TextureSize(s, 256, 256) && StageKey(s) == 0xa0aa68c1c1fc6b94ull) {
        if (const uint8_t* tc = StreamOf(s, v, 2); tc && s.streams[2].stride >= 8) {
            const float* t = reinterpret_cast<const float*>(tc);
            dark = Near(t[0] * 256, 3, 1.5f) || Near(t[0] * 256, 67, 1.5f);
            dark = dark && (Near(t[1] * 256, 231, 1.5f) || Near(t[1] * 256, 255, 1.5f));
        }
    }
    if ((!glow && !dark) || !Near(x0, 117, 2) || w < 400 || h < 150) return false;
    float top = 110, bottom = y1;
    for (uint32_t i = 0; i < 4; i++) {
        float* p = At(v, stride, i, 0);
        bool left = p[0] < (x0 + x1) / 2, upper = p[1] < (y0 + y1) / 2;
        if (upper) p[1] = top;
        if (left) p[0] = upper ? 60.0f : 100.0f;
    }
    x0 = 60, y0 = top, y1 = bottom;
    if (glow && page) {
        constexpr float kBoost = 1.45f;  // the page's background a little brighter
        for (uint32_t i = 0; i < 4; i++) {
            uint8_t* ci = col + i * s.streams[1].stride;
            uint32_t c;
            std::memcpy(&c, ci, 4);
            uint32_t a = std::min<uint32_t>(255, uint32_t(float(c >> 24) * kBoost + 0.5f));
            c = (c & 0x00FFFFFFu) | (a << 24);
            std::memcpy(ci, &c, 4);
        }
    }
    if (glow && !page) {
        // the MISSION page (and the other tabs): the background stronger, and moving as on foot (where two copies of a
        // picture fade into each other): each corner's strength rises and falls in turn, so the light goes round the
        // panel. The game's is a flat light blue (a quarter's strength), barely there.
        constexpr float kStrength = 2.6f, kSwing = 0.35f, kSpeed = 0.9f;
        constexpr float kPhase[4] = {0, 1.5708f, 4.7124f, 3.1416f};  // the fan's corners: top left, top right, bottom left, bottom right
        float t = float(double(NowMs() % 600000) / 1000.0);
        for (uint32_t i = 0; i < 4; i++) {
            uint8_t* ci = col + i * s.streams[1].stride;
            uint32_t c;
            std::memcpy(&c, ci, 4);
            float k = kStrength * (1 + kSwing * std::sin(t * kSpeed + kPhase[i]));
            uint32_t a = std::min<uint32_t>(255, uint32_t(float(c >> 24) * k + 0.5f));
            c = (c & 0x00FFFFFFu) | (a << 24);
            std::memcpy(ci, &c, 4);
        }
    }
    return false;
}

// The controller layout of a pause Controls page (driving and rails): Back to the page's list is the game's own B
// prompt, as "Are you sure?" has it (B and BACK in the prompts' letters and colour, where that question puts them, the
// B picture or the keyboard's Backspace key), not words of this port's in the bar. The game draws a B prompt on every
// pause screen, CONTINUE beside the B picture at x 110, see-through (alpha 0) where it is not wanted: that one is
// made B BACK. The letters' places and parts of the lettering's 256x256 sheet (f3b5a8f2) are the question's: B, A, C
// and K 19, 27.1, 37.2 and 45.4 right of the picture's left, 15.3 tall at the bar's other words' line, the sheet's
// texels a tenth smaller; the rest of CONTINUE's letters go. The colour is the bar's other words' (SELECT's) of the
// frame before. True: the quad is not drawn.
constexpr float kLayoutBackX = 110 + 33;  // the B picture's left, as the question's (DrivingPauseDraw)
std::atomic<uint64_t> g_layout_back_seen{0};
std::atomic<uint32_t> g_prompt_word_colour{0xFF28BAC2u};
bool DrivingLayoutBack(const gpu::State& s, uint8_t* v, uint32_t stride, float& x0, float& y0, float& x1, float& y1) {
    static uint64_t letters_frame = ~0ull;
    static int letters = 0;
    if (!TextureSize(s, 256, 256) || y0 < 376 || y1 > 412 || x1 - x0 > 24) return false;
    uint8_t* col = StreamOf(s, v, 1);
    uint8_t* tc = StreamOf(s, v, 2);
    if (!col || !tc || s.streams[1].stride < 4 || s.streams[2].stride < 8) return false;
    uint32_t c;
    std::memcpy(&c, col, 4);
    const uint64_t key = StageKey(s);
    const bool letter = key == 0xf3b5a8f28d4357d7ull;
    // the bar's words in sight: their colour for B BACK in the next frame
    if (letter && (c >> 24) >= 0x80 && Channels(c) >= 60 && x0 > 240) g_prompt_word_colour.store(c);
    float t0, t1;
    if ((c >> 24) != 0 || x0 < 100 || x0 > 240 || !LayoutTab(t0, t1)) return false;
    const uint32_t word = g_prompt_word_colour.load();
    auto set = [&](float nx0, float nx1, float nu0, float nv0, float nu1, float nv1, uint32_t colour) {
        for (uint32_t i = 0; i < 4; i++) {
            float* p = At(v, stride, i, 0);
            bool right = p[0] > (x0 + x1) / 2, lower = p[1] > (y0 + y1) / 2;
            p[0] = right ? nx1 : nx0;
            float* t = reinterpret_cast<float*>(tc + i * s.streams[2].stride);
            t[0] = (right ? nu1 : nu0) / 256, t[1] = (lower ? nv1 : nv0) / 256;
            std::memcpy(col + i * s.streams[1].stride, &colour, 4);
        }
        x0 = nx0, x1 = nx1;
    };
    if (key == 0xa0aa68c1c1fc6b94ull) {
        // the B picture (its part of the sheet: 99,34 to 131,98)
        const float* t = reinterpret_cast<const float*>(tc);
        if (!Near(x0, 110, 4) || !Near(std::min(t[0], reinterpret_cast<const float*>(tc + s.streams[2].stride)[0]) * 256, 99, 2))
            return false;
        set(kLayoutBackX, kLayoutBackX + (x1 - x0), 99, 34, 131, 98, (word & 0xFF000000u) | 0x00FFFFFFu);
        g_layout_back_seen.store(Frame() + 1);
        return false;
    }
    if (!letter) return false;
    uint64_t f = Frame();
    if (f != letters_frame) letters_frame = f, letters = 0;
    int k = letters++;
    if (k >= 4) return true;  // the rest of CONTINUE
    struct Letter {
        float dx, w, u0, v0, u1, v1;
    };
    static constexpr Letter kBack[4] = {{19.0f, 11.7f, 218, 108, 231, 125},
                                        {27.1f, 13.5f, 184, 186, 199, 203},
                                        {37.2f, 10.8f, 45, 58, 57, 75},
                                        {45.4f, 11.7f, 30, 172, 43, 189}};
    const Letter& l = kBack[k];
    set(kLayoutBackX + l.dx, kLayoutBackX + l.dx + l.w, l.u0, l.v0, l.u1, l.v1, word);
    return false;
}

void DrivingPauseDraw(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    constexpr uint32_t kFan = 6;
    if (prim != kFan || count != 4 || stride < 16) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < 4; i++) {
        const float* p = At(v, stride, i, 0);
        x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
    }
    bool page = PausePage();
    if (page && (Paused() || Closing())) PageTreated();
    auto collapse = [&] {
        for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[0] = x0, At(v, stride, i, 0)[1] = y0;
    };
    if ((Paused() || Closing()) && DrivingPanelPiece(s, v, stride, page, x0, y0, x1, y1)) {
        collapse();
        return;
    }
    // a pause page up: the panel without its words and figure, taller (the rows and the marker are not drawn at all
    // then, src/driving/pause.cpp)
    // the running figure (a 128x128 picture right of the rows, reaching under the panel)
    if (page && (Paused() || Closing()) && TextureSize(s, 128, 128) && x1 > 380 && y1 > 120) {
        collapse();
        return;
    }
    if (page && (Paused() || Closing()) && InPanel(kDrivePanel, x0, y0, y1)) {
        // the Lives Remaining bar's box and its top line (its words gone): a grey band with a light line across the
        // page above the prompts' bar; the page's moving background runs down to that bar instead (DrivingPageGlow)
        if (!s.textures[0] && x0 >= 155 && x1 <= 565 && y0 >= 282 && y1 <= 311) {
            collapse();
            return;
        }
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
    // "Are you sure?" up: B BACK (the B picture at x 110, its word and the word's shadow) nearer SELECT, as far from
    // its A picture as SELECT is from the prompt after it on the MISSION page (49 pixels); it sat out by the panel's
    // curve, 82 from it
    if ((Paused() || Closing()) && !page && Recent(g_drive_seen.load()) && g_drive_lit.load() < 0 && s.textures[0] &&
        KeepsSize(x1 - x0, y1 - y0) && x0 >= 105 && x1 <= 205 && y0 > 311 && y1 < 340) {
        constexpr float kBackShift = 33;
        for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[0] += kBackShift;
        x0 += kBackShift, x1 += kBackShift;
    }
    // the controller layout of the Controls page: B BACK in the prompts' bar, the game's own prompt (DrivingLayoutBack)
    if ((Paused() || Closing()) && page && DrivingLayoutBack(s, v, stride, x0, y0, x1, y1)) {
        collapse();
        return;
    }
    // a button prompt (coordinates in stream 2)
    if (const uint8_t* tc = StreamOf(s, v, 2); tc && s.streams[2].stride >= 8) {
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (uint32_t i = 0; i < 4; i++) {
            const float* t = reinterpret_cast<const float*>(tc + i * s.streams[2].stride);
            u0 = std::min(u0, t[0]), u1 = std::max(u1, t[0]), v0 = std::min(v0, t[1]), v1 = std::max(v1, t[1]);
        }
        // a pause screen's prompt picture on its words' middle line (each picture fills its quad)
        if ((Paused() || Closing()) && PromptPicture(s, u0, v0, u1, v1)) {
            float dy = PromptToWords(x0, y0, x1, y1, (y0 + y1) / 2);
            for (uint32_t i = 0; i < 4; i++) At(v, stride, i, 0)[1] += dy;
            y0 += dy, y1 += dy;
        }
        float alpha = 1;
        if (const uint8_t* col = StreamOf(s, v, 1); col && s.streams[1].stride >= 4) {
            uint32_t c;
            std::memcpy(&c, col, 4);
            alpha = float(c >> 24) / 255.0f * float(std::max({(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF})) / 255.0f;
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
    // a letter of the question's Yes or No, for the mouse
    if (y1 - y0 < 40 && x1 - x0 < 40) AnswerLetter(x0, y0, x1, y1);
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
constexpr uint64_t kXButton = 0x3914e9949f3a46d1ull;
// the X button keyed (REMATCH, a multiplayer game's results), the frame plus one: Backspace gives X meanwhile
std::atomic<uint64_t> g_rematch_seen{0};
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
    // the D-pad with one arm lit, drawn as it is (NEXT) and mirrored (PREVIOUS): the multiplayer game select
    {0x7ffff33c7f8a7768ull, 58, 89, 81, 112, Prompt::LeftRight},
    // the X button (32x64), REMATCH on a multiplayer game's results: BACKSPACE, which gives X there (Pad)
    {kXButton, -1, -1, -1, -1, Prompt::B},
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
    bool pause = false;    // a pause menu's (drawn while paused, its last frame too)
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
// driving.xbe quitting a mission (src/driving/hooks.cpp): the frames from the answer on, and when the last one ended
std::atomic<uint64_t> g_hold_seen{0};
std::atomic<int64_t> g_hold_at{0};
// and each of those frames with how much of the pause screen's picture it shows (1 the picture whole, before the fade
// starts; the fades drawn onto that frame's buffer darken it, src/driving/hooks.cpp). The prompts the pause screen
// showed last are kept for them (the game shows its picture again, not its prompts).
struct HoldFrame {
    uint64_t frame = ~0ull;
    float left = 1;
};
std::array<HoldFrame, 8> g_hold_frames;
std::vector<PromptRect> g_prompts_held;

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

// a prompt's picture (one the keys stand in for) is drawn with these coordinates (0 to 1)
bool PromptPicture(const gpu::State& s, float u0, float v0, float u1, float v1) {
    uint64_t key = StageKey(s);
    if (!key) return false;
    gpu::TextureDesc d;
    gpu::Describe(s.textures[0], d);
    float tu0 = u0 * float(d.width), tu1 = u1 * float(d.width), tv0 = v0 * float(d.height), tv1 = v1 * float(d.height);
    for (const PromptPart& p : kPrompts)
        if (p.key == key && (p.u0 < 0 || (Near(tu0, p.u0, 2) && Near(tu1, p.u1, 2) && Near(tv0, p.v0, 2) && Near(tv1, p.v1, 2))))
            return true;
    return false;
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
        if (key == kXButton) {
            // only REMATCH beside CONTINUE (A before it, no B in the frame): the one X the keyboard has a key for
            bool a = false, b = false;
            for (const PromptRect& r : fp.list) a |= r.prompt == Prompt::A, b |= r.prompt == Prompt::B;
            if (!a || b) return false;
            g_rematch_seen.store(f + 1);
        }
        if (fp.list.size() < 64) fp.list.push_back({p.prompt, x0, y0, x1, y1, std::clamp(alpha, 0.0f, 1.0f), DrawPaused()});
        g_prompts_seen.store(f + 1);
        return Keyboard();
    }
    return false;
}

// something drawn over the whole screen in this frame (a loading picture, a fade to black) leaving this much of what
// was drawn before it: the prompts and the setup page drawn so far in the frame fade or go with it
void Covered(float left) {
    left = std::clamp(left, 0.0f, 1.0f);
    std::lock_guard l(g_prompt_lock);
    uint64_t f = Frame();
    FramePrompts& fp = g_prompt_frames[f % g_prompt_frames.size()];
    if (fp.frame == f)
        for (PromptRect& p : fp.list) p.alpha *= left;
    FrameLevel& sl = g_setup_level[f % g_setup_level.size()];
    if (sl.frame == f) sl.level *= left;
}

// an opaque quad drawn over part of the screen: the prompts drawn so far in the frame under it go (the multiplayer
// screens draw a prompts' bar off to the right, then black over it, then their own)
void CoveredBox(const Box& b) {
    std::lock_guard l(g_prompt_lock);
    uint64_t f = Frame();
    FramePrompts& fp = g_prompt_frames[f % g_prompt_frames.size()];
    if (fp.frame != f) return;
    for (PromptRect& p : fp.list) {
        float cx = (p.x0 + p.x1) / 2, cy = (p.y0 + p.y1) / 2;
        if (cx >= b.x0 && cx <= b.x1 && cy >= b.y0 && cy <= b.y1) p.alpha = 0;
    }
}

// bond_eng.xbe: a 2D draw of one quad from the screen's top left corner that hides what is under it: the whole screen
// (a picture, or a black fade of its own opacity), or the first tile of a full screen picture (the loading screens)
void CoverDraw(const gpu::State& s, uint32_t prim, const uint8_t* v, uint32_t stride, uint32_t count) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (prim != kTriangleList || count != 6 || !s.vs || !s.vs->program.empty()) return;
    if (std::memcmp(s.transforms[3].data(), kIdentity, sizeof(kIdentity)) != 0) return;
    const auto& pos = s.vs->inputs[0];
    if (pos.stream != 0 || pos.type != kFloat4 || pos.offset + 16 > stride) return;
    int color = -1;
    for (int r = 1; r < 16; r++) {
        const auto& in = s.vs->inputs[size_t(r)];
        if (r != 9 && in.stream == 0 && in.type == kFloat4 && in.offset + 16 <= stride) color = int(in.offset);
    }
    if (color < 0) return;
    Layout l;
    l.pos = pos.offset;
    Box b = BoxOf(v, stride, l, 0, count);
    if (!s.textures[0]) {
        float least = 1;
        for (uint32_t i = 0; i < count; i++) least = std::min(least, At(v, stride, i, uint32_t(color))[3]);
        if (least >= 0.98f) CoveredBox(b);
    }
    if (b.x0 > 1 || b.y0 > 1) return;
    std::array<float, 4> c;
    std::memcpy(c.data(), v + uint32_t(color), 16);
    bool whole = b.x1 >= 639 && b.y1 >= 479, tile = b.x1 - b.x0 >= 128 && b.y1 - b.y0 >= 128;
    if (s.textures[0]) {
        if ((whole || tile) && c[3] >= 0.9f) Covered(0);
    } else if (whole && std::max({c[0], c[1], c[2]}) < 0.1f && c[3] > 0.02f) {
        Covered(1 - c[3]);
    }
}

// the setup page's title drawn in this frame, in this colour
void SetupLevel(const std::array<float, 4>& c) {
    std::lock_guard l(g_prompt_lock);
    uint64_t f = Frame();
    FrameLevel& sl = g_setup_level[f % g_setup_level.size()];
    float level = std::clamp(c[3], 0.0f, 1.0f) * std::clamp(std::max({c[0], c[1], c[2]}), 0.0f, 1.0f);
    if (sl.frame != f) sl.frame = f, sl.level = 0;
    sl.level = std::max(sl.level, level);
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
            // the middle of the letters themselves: driving.xbe's sit in the lower part of their quads (a 15.3 pixel
            // quad, the letters' middle 2 below its own), bond_eng.xbe's in the upper (0.7 above its middle)
            constexpr float kInk = kDrivingImage ? 0.135f : -0.06f;
            if (mid) *mid = (r.y0 + r.y1) / 2 + (r.y1 - r.y0) * kInk;
            return true;
        }
    return false;
}

// how far a pause menu's prompt picture (x0..y1 its quad, picture_mid the middle line of what it shows, console
// pixels) moves down to sit on its words' middle line, by the words of the frame before (0 without words beside it).
// The game's sat a little high: on foot the D-pads 2 pixels and A 1, driving and rails A and B 2.5 to 3.5 and the
// D-pads 1, which the PC's resolutions show.
float PromptToWords(float x0, float y0, float x1, float y1, float picture_mid) {
    float mid = 0;
    if (!Labelled({Prompt::A, x0, y0, x1, y1}, &mid)) return 0;
    float dy = mid - picture_mid;
    return std::abs(dy) < 6 ? dy : 0;
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
    const bool own = fp.frame == shown && !fp.list.empty();
    // quitting a driving or on-rails mission (from the frame Quit Mission's Yes is taken, src/driving/hooks.cpp): the
    // pause screen's picture stays up and fades to black, its words with it, so the keys (the prompts it last showed)
    // are as bright as the picture that frame shows; none once it is all but black (or after the fade), and none once
    // the game has stopped ending frames for the quit
    if (Recent(g_hold_seen.load()) || (g_hold_at.load() && NowMs() - g_hold_at.load() < 2000)) {
        if (NowMs() - g_hold_at.load() > 250) return {};
        const HoldFrame& hf = g_hold_frames[shown % g_hold_frames.size()];
        if (hf.frame == shown) {
            if (hf.left < 0.03f) return {};
            std::vector<PromptRect> held = own ? fp.list : g_prompts_held;
            for (PromptRect& p : held) p.alpha *= hf.left;
            return held;
        }
        // a frame from before the answer (still on its way to the screen): its own prompts, as before
        if (!own) return {};
    }
    if (own) {
        g_prompts_held = fp.list;
        return fp.list;
    }
    return {};
}

// the pre-mission Controller Setup shows in the frame being shown (its title drawn, and not covered or faded out)
[[maybe_unused]] bool SetupShows() {
    std::lock_guard l(g_prompt_lock);
    uint64_t shown = app::g_shown_frame.load();
    const FrameLevel& sl = g_setup_level[shown % g_setup_level.size()];
    return sl.frame == shown && sl.level > 0.1f;
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
    g_page_from.store(0);
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

// A multiplayer match's respawn screen (the scores, "Press A to Join" under them) is drawn under the level's projection,
// so its A button was neither keyed nor hidden. When it is drawn in the view of the player port 0 (the keyboard and
// mouse) plays, it is a prompt as any other, and Enter (Space, a left click) gives that player A meanwhile (RespawnUp).
std::atomic<uint64_t> g_respawn_seen{0};

// the screen rectangle (console pixels) of the view of the local player port 0 plays; false when none plays it
bool PortZeroView(float& x0, float& y0, float& x1, float& y1) {
    // the local players: those of [0x205408] (bots counted too) with a port in [0x218464 + p*4] (-1 for a bot)
    constexpr uint32_t kPlayers = 0x205408, kPorts = 0x218464;
    int32_t all = std::min(int32_t(kernel::R32(kPlayers)), 4), players = 0, p = -1;
    for (int32_t i = 0; i < all; i++) {
        uint32_t port = kernel::R32(kPorts + uint32_t(i) * 4);
        if (port > 3) continue;
        if (port == 0) p = players;
        players++;
    }
    if (all <= 1) p = 0, players = 1;
    if (p < 0) return false;
    x0 = 0, y0 = 0, x1 = 640, y1 = 480;
    if (players == 2) y0 = p ? 240.0f : 0.0f, y1 = y0 + 240;
    if (players >= 3) x0 = p % 2 ? 320.0f : 0.0f, x1 = x0 + 320, y0 = p / 2 ? 240.0f : 0.0f, y1 = y0 + 240;
    return true;
}

void RespawnPrompt(const gpu::State& s, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    if (count != 6 || controls::CurrentGameState() != controls::GameState::Playing || !TextureSize(s, 32, 64) ||
        StageKey(s) != 0x94de8205578ce49full)
        return;
    Layout l = MenuLayout(s, prim, stride, count, true);
    if (!l.ok) return;
    Box b = BoxOf(v, stride, l, 0, count);
    float x0, y0, x1, y1, cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
    if (!PortZeroView(x0, y0, x1, y1) || cx < x0 || cx > x1 || cy < y0 || cy > y1) return;
    g_respawn_seen.store(Frame() + 1);
    auto pc = ColorOf(v, stride, l);
    if (PromptQuad(s, b.x0, b.y0, b.x1, b.y1, b.u0, b.v0, b.u1, b.v1, pc[3] * std::clamp(std::max({pc[0], pc[1], pc[2]}), 0.0f, 1.0f)))
        Hide(v, stride, l, count);
}

// ---------------------------------------------------------------------------------------------------------------

void Draw(const void* state, uint32_t prim, uint8_t* v, uint32_t stride, uint32_t count) {
    if (kDrivingImage) {
        DrivingPauseDraw(*static_cast<const gpu::State*>(state), prim, v, stride, count);
        return;
    }
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    CoverDraw(s, prim, v, stride, count);
    if ((DrawPaused() || Closing()) && PausePage() && PausePanelDraw(s, prim, v, stride, count)) return;
    // the pause menu's question: its band moved up with the rows
    if (DrawPaused() && !PausePage() && QuestionDrawn() && QuestionBand(s, v, stride, count)) return;
    if (DrawPaused() || Closing()) FootTabBar(s, prim, v, stride, count);
    Layout l = MenuLayout(s, prim, stride, count);
    if (!l.ok) {
        RespawnPrompt(s, prim, v, stride, count);
        return;
    }
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

    // the pause menu's B BACK prompt: the question is up (it is drawn before the rows)
    if (count == 6 && DrawPaused() && !PausePage() && TextureSize(s, 32, 64) && StageKey(s) == 0x08e4f0ae95c16b80ull &&
        b.y0 > 300 && b.y1 < 345)
        g_question_seen.store(Frame() + 1);

    // a pause menu's prompt picture on its words' middle line, and the two D-pad pictures (the pause menu's own 32x32
    // sheet) the same size, 17 by 16 (CHANGE PAGE's was drawn a pixel wider than SCROLL's); what they show sits half a
    // texel above their quad's middle
    if (count == 6 && DrawPaused() && PromptPicture(s, b.u0, b.v0, b.u1, b.v1)) {
        bool pad = StageKey(s) == 0x7e8cc44b3a0258fcull;
        if (pad && b.x1 - b.x0 > 17.5f) {
            float cx = (b.x0 + b.x1) / 2;
            SpanX(v, stride, l, count, cx - 8.5f, cx + 8.5f);
        }
        float pic = (b.y0 + b.y1) / 2 - (pad ? (b.y1 - b.y0) / 30 : 0);
        MoveY(v, stride, l, count, PromptToWords(b.x0, b.y0, b.x1, b.y1, pic));
        b = BoxOf(v, stride, l, 0, count);
    }
    // a button prompt: the keyboard's key in its place while the keyboard and mouse play
    // (how much of it shows: its colour's alpha, and its brightness, should the game darken it to black instead)
    auto pc = ColorOf(v, stride, l);
    float shows = pc[3] * std::clamp(std::max({pc[0], pc[1], pc[2]}), 0.0f, 1.0f);
    if (count == 6 && PromptQuad(s, b.x0, b.y0, b.x1, b.y1, b.u0, b.v0, b.u1, b.v1, shows)) {
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
    if (SetupTitle(s, b, count, v, stride, l)) {
        g_setup_seen.store(Frame() + 1);
        g_setup_frame = Frame();
        if (!IsShadow(ColorOf(v, stride, l))) SetupLevel(ColorOf(v, stride, l));
    } else if (count == 6 && g_setup_frame == Frame() && b.x0 >= 40 && b.y0 >= 120 && b.y0 < 400 &&
               b.x1 <= 620 && b.y1 <= 470) {
        // every picture after the title, the labels and their lines too (the driving and rail pages use other
        // sheets than the on-foot one), above the footer
        Hide(v, stride, l, count);
        return;
    }

    // the pause menu (and its last frame as the game shuts it)
    if (DrawPaused()) {
        bool page = PausePage();
        // the frame's curved left column: 32x32 pictures side by side, each drawn whole. The filtering at a picture's
        // edge took in its opposite edge (the texture wraps): lines between the pieces. Each picture's edges go half a
        // texel in (the same way however often the game draws it).
        if (count == 6 && TextureSize(s, 32, 32) && b.x0 >= 28 && b.x1 <= 132 && b.y0 >= 50 && b.y1 <= 430 &&
            b.u1 - b.u0 > 0.9f && b.v1 - b.v0 > 0.9f) {
            InsetUV(v, stride, l, count, 32);
            return;
        }
        // a page's moving light-blue background (the 64x64 picture drawn twice over the whole panel) a little brighter;
        // its red, 1 as the game draws it, marks it as done (a line the game keeps is brightened once)
        if (page && count == 6 && TextureSize(s, 64, 64) && b.x1 - b.x0 > 400 && b.y1 - b.y0 > 200) {
            constexpr float kBoost = 1.3f, kDone = 1.0f - 1.0f / 512;
            for (uint32_t i = 0; i < count; i++) {
                float* c = At(v, stride, i, l.color);
                if (c[0] == kDone) continue;
                c[0] = kDone, c[3] = std::min(1.0f, c[3] * kBoost);
            }
            return;
        }
        if (count == 6 && TextureSize(s, 64, 64) && Near(b.x0, kMarkerX, 3.5f)) {
            // the marker is the << of a 64x64 sheet (drawn mirrored) whose quad starts on the texel row right under
            // the sheet's blue picture and ends at its right edge: the filtering took in a row of that picture (a
            // line along the marker's top) and, wrapping, the sheet's left edge. Each edge of the part drawn goes
            // half a texel in, the same way however often the game draws it (as the driving HUD's sheet).
            if (!MoveMarker(v, stride, l)) InsetUV(v, stride, l, 6, 64);
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
            // "Are you sure?" up: only Continue Mission and the question's own line (letters and shadow), the line
            // drawn after its band
            int asked = QuestionRow();
            bool other = !page && QuestionDrawn() && row > 0 && (asked < 0 || row != asked);
            if (page || other) Hide(v, stride, l, count);
            return;
        }
        // the question's Yes and No, on its row's line: moved up with it
        if (!page && QuestionDrawn()) {
            for (float x : {293.0f, 352.0f}) {
                bool up = false;
                if (PauseRow(s, b, &up, x) < 0) continue;
                if (!up) MoveY(v, stride, l, count, -kPauseLift);
                // where the mouse finds it (MouseMenus), and how bright: the lit answer is the brighter
                float dy = up ? 0 : -kPauseLift;
                auto c = ColorOf(v, stride, l);
                AnswerQuad(x < 300 ? 0 : 1, b.x0, b.y0 + dy, b.x1, b.y1 + dy, IsShadow(c) ? 0 : Brightness(c));
                return;
            }
        }
    }

    // the mission select's 007 panels either side of the mission's picture: a click on one is LEFT or RIGHT
    if (count == 6 && !DrawPaused() && TextureSize(s, 256, 256) && b.y0 > 100 && b.y1 < 280 && b.x1 - b.x0 > 90 &&
        b.x1 - b.x0 < 200 && StageKey(s) == 0xb6da7d3f2f3aa462ull)
        SidePanel((b.x0 + b.x1) / 2 < 320 ? 0 : 1, b.x0, b.y0, b.x1, b.y1);

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

// The on-foot pause panel's moving light-blue background (two 64x64 pictures over x 98 to 640, y 111 to 318 before a
// page's stretch) starts at x 98, while the frame's curve (the column of pictures at x 64 to 128) has its inner line
// further left on most rows (x 42 at its deepest): between the two the scene showed with no background over it, a
// lighter strip with a hard edge down the panel at x 98. GlowToCurve fills it: a strip of the background's first
// column (its picture, colour and rows as at its left edge, so it meets the quad with no edge) from the curve's inner
// line (kFootCurve, measured on the game's own frame a pixel inside the line, rows in the background's height 0 to 1)
// to x 98, under the curve's pictures (drawn after it). Where the line is right of 98 (the bottom rows) nothing.
constexpr float kFootGlowX = 98;
constexpr float kFootCurve[][2] = {{0.000f, 77}, {0.039f, 67}, {0.077f, 59}, {0.116f, 52}, {0.155f, 47}, {0.193f, 45},
                                   {0.232f, 44}, {0.271f, 43}, {0.309f, 43}, {0.348f, 44}, {0.386f, 46}, {0.425f, 48},
                                   {0.464f, 51}, {0.502f, 54}, {0.541f, 57}, {0.580f, 61}, {0.618f, 64}, {0.657f, 67},
                                   {0.696f, 71}, {0.734f, 77}, {0.773f, 82}, {0.812f, 87}, {0.850f, 91}, {0.889f, 96},
                                   {0.928f, 100}};
std::vector<uint8_t> GlowToCurve(const uint8_t* v, uint32_t stride, const Layout& l, const Box& b) {
    // the quad's left edge: its top and bottom vertices
    int top = -1, bottom = -1;
    for (uint32_t i = 0; i < 6; i++) {
        const float* p = At(v, stride, i, l.pos);
        if (!Near(p[0], b.x0, 0.5f)) continue;
        if (Near(p[1], b.y0, 0.5f)) top = int(i);
        if (Near(p[1], b.y1, 0.5f)) bottom = int(i);
    }
    std::vector<uint8_t> out;
    if (top < 0 || bottom < 0) return out;
    const float* ct = At(v, stride, uint32_t(top), l.color);
    const float* cb = At(v, stride, uint32_t(bottom), l.color);
    const float* tt = At(v, stride, uint32_t(top), l.tc);
    const float* tb = At(v, stride, uint32_t(bottom), l.tc);
    auto vertex = [&](float x, float f) {
        size_t at = out.size();
        out.insert(out.end(), v + size_t(top) * stride, v + size_t(top + 1) * stride);
        float* p = reinterpret_cast<float*>(out.data() + at + l.pos);
        float* c = reinterpret_cast<float*>(out.data() + at + l.color);
        float* t = reinterpret_cast<float*>(out.data() + at + l.tc);
        p[0] = x, p[1] = b.y0 + (b.y1 - b.y0) * f;
        for (int k = 0; k < 4; k++) c[k] = ct[k] + (cb[k] - ct[k]) * f;
        t[0] = tt[0], t[1] = tt[1] + (tb[1] - tt[1]) * f;
    };
    constexpr int n = int(sizeof(kFootCurve) / sizeof(kFootCurve[0]));
    for (int i = 0; i + 1 < n; i++) {
        float f0 = kFootCurve[i][0], f1 = kFootCurve[i + 1][0];
        float x0 = std::min(kFootCurve[i][1], b.x0), x1 = std::min(kFootCurve[i + 1][1], b.x0);
        if (x0 >= b.x0 && x1 >= b.x0) continue;
        vertex(x0, f0), vertex(b.x0, f0), vertex(x1, f1);
        vertex(x1, f1), vertex(b.x0, f0), vertex(b.x0, f1);
    }
    return out;
}

void AfterDraw(const void* state, uint32_t prim, const uint8_t* v, uint32_t stride, uint32_t count, app::GameInfo::Emit emit) {
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    // the controller layout's Keyboard & Mouse tab selected: the tabs' bar dark from its start to the light box too (a
    // copy of the dark rest of the bar, which Draw started at the box's right end)
    if (float t0, t1; prim == kTriangleList && (DrawPaused() || Closing()) && LayoutTab(t0, t1) && t0 > kFootTabsLeft + 1 &&
                      s.vs && s.vs->inputs[0].stream == 0 && s.vs->inputs[0].type == kFloat4) {
        Layout tl;
        tl.pos = s.vs->inputs[0].offset;
        if (tl.pos + 16 <= stride && FootTabPiece(s, v, stride, count, tl) == 2) {
            std::vector<uint8_t> left(v, v + size_t(count) * stride);
            SpanX(left.data(), stride, tl, count, kFootTabsLeft, t0);
            emit(prim, left.data(), count);
            return;
        }
    }
    Layout l = MenuLayout(s, prim, stride, count);
    if (!l.ok) return;
    Box b = BoxOf(v, stride, l, 0, count);

    // the pause panel's moving light-blue background: on to the frame's curve
    if (count == 6 && prim == kTriangleList && (DrawPaused() || Closing()) && TextureSize(s, 64, 64) &&
        Near(b.x0, kFootGlowX, 1) && b.x1 - b.x0 > 400 && b.y1 - b.y0 > 150) {
        std::vector<uint8_t> fill = GlowToCurve(v, stride, l, b);
        if (!fill.empty()) emit(prim, fill.data(), uint32_t(fill.size() / stride));
        return;
    }

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
    // (not while the game asks "Are you sure?": its menu is the game's alone then, as it draws it)
    if (count >= 6 && DrawPaused() && PauseRow(s, b) == kPauseRows - 1 && !PausePage() && !QuestionDrawn()) {
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
    // (a pause row's shadow: nothing more after it)
    if (count >= 6 && DrawPaused() && !PausePage() && PauseRow(s, b) >= 0 && IsShadow(ColorOf(v, stride, l))) return;

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
bool RespawnUp() { return kMenus && Recent(g_respawn_seen.load()); }

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

void HoldPrompts() {
    {
        std::lock_guard l(g_prompt_lock);
        uint64_t f = Frame();
        HoldFrame& hf = g_hold_frames[f % g_hold_frames.size()];
        if (hf.frame != f) hf = {f, 1.0f};  // until ScreenFade says otherwise
    }
    g_hold_seen.store(Frame() + 1);
    g_hold_at.store(NowMs());
}
void ScreenFade(float left) {
    std::lock_guard l(g_prompt_lock);
    uint64_t f = Frame();
    if (!Recent(g_hold_seen.load())) return;  // not a quit's
    g_hold_frames[f % g_hold_frames.size()] = {f, std::clamp(left, 0.0f, 1.0f)};
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
// The mouse cursor on the game's own menus. controls.cpp shows the Windows arrow on a menu (the front end, a pause
// menu) once the mouse has moved there, and hides it again in play. With it shown:
// - the row under the cursor is selected when the cursor moves onto it, and a left click on a row accepts it, as
//   ENTER does: the main menu's bars (QUIT GAME sixth), the options list's lines (PC Settings third), the pause menus'
//   MISSION rows (Controls and PC Settings fourth and fifth) and the "Are you sure?" question's Yes and No;
// - a click on a prompt's A or B picture, or its words, presses that button (B BACK goes back); on the mission
//   select a click on the 007 panel left or right of the mission's picture is LEFT or RIGHT (the mission before or
//   after);
// - anywhere else a click does nothing on a screen with those rows, and is A on any other screen (the briefing, the
//   pre-mission Controller Setup, the mission select), as before.
// The cursor itself does not move the selection, only its moving onto a row does. Ui hands the cursor over in console
// pixels (MouseCursor); the pad then gets D-pad presses toward the row, one row a press, and A once there, as a
// player would press them, so the added rows go through the same code as the controller's. Nothing of it in play (the
// mouse buttons are the game's there) or while a page is up (the pages take the mouse themselves).

enum class MouseMenu { None, Main, Options, Pause, Question };

std::atomic<float> g_cursor_x{-1}, g_cursor_y{-1};  // console pixels
std::atomic<bool> g_cursor_over{false};             // over the console's frame
std::atomic<uint32_t> g_cursor_moves{0}, g_cursor_clicks{0};
std::atomic<int64_t> g_cursor_click_at{0};

// the "Are you sure?" question's two answers as the game last drew them (bond_eng.xbe: Draw; driving.xbe: the words
// between DrivingQuestionWord calls), each its box in console pixels, how bright it was drawn and the frame
struct AnswerBox {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    float bright = 0;
    uint64_t seen = 0;  // frame plus one
};
std::mutex g_answer_lock;
std::array<AnswerBox, 2> g_answers;
std::atomic<int> g_answer_open{-1};  // driving.xbe: the answer being drawn now

void AnswerQuad(int i, float x0, float y0, float x1, float y1, float bright) {
    if (i < 0 || i > 1) return;
    std::lock_guard l(g_answer_lock);
    AnswerBox& a = g_answers[size_t(i)];
    if (a.seen != Frame() + 1) a = AnswerBox{}, a.seen = Frame() + 1;
    a.x0 = std::min(a.x0, x0), a.y0 = std::min(a.y0, y0), a.x1 = std::max(a.x1, x1), a.y1 = std::max(a.y1, y1);
    a.bright = std::max(a.bright, bright);
}

void AnswerLetter(float x0, float y0, float x1, float y1) {
    if (g_answer_open.load() < 0) return;
    std::lock_guard l(g_answer_lock);
    int i = g_answer_open.load();
    if (i < 0 || i > 1) return;
    AnswerBox& a = g_answers[size_t(i)];
    a.x0 = std::min(a.x0, x0), a.y0 = std::min(a.y0, y0), a.x1 = std::max(a.x1, x1), a.y1 = std::max(a.y1, y1);
}

std::array<AnswerBox, 2> g_sides;  // the mission select's side panels (g_answer_lock)
void SidePanel(int i, float x0, float y0, float x1, float y1) {
    std::lock_guard l(g_answer_lock);
    g_sides[size_t(i)] = {x0, y0, x1, y1, 0, Frame() + 1};
}

// the mission select's side panel under (x, y): 0 left, 1 right, -1 none
int SideAt(float x, float y) {
    std::lock_guard l(g_answer_lock);
    for (int i = 0; i < 2; i++) {
        const AnswerBox& a = g_sides[size_t(i)];
        if (Recent(a.seen) && x >= a.x0 && x <= a.x1 && y >= a.y0 && y <= a.y1) return i;
    }
    return -1;
}

// both answers drawn lately: their boxes, and the lit one (the brighter), else -1
int Answers(std::array<AnswerBox, 2>& out) {
    std::lock_guard l(g_answer_lock);
    out = g_answers;
    static int lit = 0;
    if (!Recent(out[0].seen) || !Recent(out[1].seen)) return -1;
    // the lit one pulses: when the two come near, the last that stood out
    if (std::fabs(out[0].bright - out[1].bright) >= 0.1f) lit = out[0].bright > out[1].bright ? 0 : 1;
    return lit;
}

// the menu up and its selected row (the main menu 0 to 5 with QUIT GAME; the options list 0 to 10 with PC Settings
// third; the pause menu 0 to 4 with Controls and PC Settings; the question's answer 0 or 1), -1 when that menu is not up
int MouseRowNow(MouseMenu m) {
    switch (m) {
        case MouseMenu::Main:
            if (!kMenus || !Recent(g_main_seen.load()) || g_page.load() != Page::None) return -1;
            return g_quit_selected.load() ? kMainItems : g_main_selected.load();
        case MouseMenu::Options: {
            if (!kMenus || !Recent(g_options_seen.load()) || g_page.load() != Page::None) return -1;
            int sel = g_options_selected.load();
            if (g_pc_selected.load()) return kControllerRow + 1;
            return sel < 0 ? -1 : sel <= kControllerRow ? sel : sel + 1;
        }
        case MouseMenu::Pause: {
            if (!Paused() || g_page.load() != Page::None) return -1;
            bool up = kDrivingImage ? DriveMissionPage() : Recent(g_pause_seen.load());
            if (!up || (kDrivingImage ? g_drive_lit.load() < 0 : PauseQuestion())) return -1;
            int extra = g_pause_extra.load();
            if (extra >= 0) return kPauseRows + extra;
            return kDrivingImage ? g_drive_lit.load() : g_pause_lit.load();
        }
        case MouseMenu::Question: {
            if (!Paused() || g_page.load() != Page::None) return -1;
            bool asked = kDrivingImage ? Recent(g_drive_seen.load()) && g_drive_lit.load() < 0 : PauseQuestion();
            std::array<AnswerBox, 2> a;
            return asked ? Answers(a) : -1;
        }
        default: return -1;
    }
}

// the menu with rows up now, and its selected row (-1 and None when none)
int MenuNow(MouseMenu& menu) {
    for (MouseMenu m : {MouseMenu::Question, MouseMenu::Pause, MouseMenu::Main, MouseMenu::Options}) {
        int cur = MouseRowNow(m);
        if (cur >= 0) {
            menu = m;
            return cur;
        }
    }
    menu = MouseMenu::None;
    return -1;
}

// the row of that menu under (x, y), console pixels, -1 none: the rows' boxes as they are drawn (moved by this port)
int MouseRowAt(MouseMenu m, float x, float y) {
    int rows = 0;
    float x0 = 0, x1 = 0, above = 0, h = 0;
    auto top = [&](int r) -> float {
        switch (m) {
            case MouseMenu::Main: return kNewTop + kNewStep * float(r);  // the bars, 30 tall
            case MouseMenu::Options:
                if (r <= kControllerRow) return kRowTop + kRowStep * float(r);
                return kRowTop + kRowStep * float(r - 1) + kInsert;  // PC Settings, then the rows moved down under it
            case MouseMenu::Pause:
                return kDrivingImage ? kDriveRowsTop + kDriveStep * float(r) : kPauseTop - kPauseLift + kPauseStep * float(r);
            default: return -1000;
        }
    };
    switch (m) {
        case MouseMenu::Main: rows = kMainItems + 1, x0 = kBarX0, x1 = kBarX1, above = 8, h = 46; break;
        case MouseMenu::Options: rows = kOptionRows + 1, x0 = kLabelX - 8, x1 = 600, above = 6, h = kRowStep; break;
        case MouseMenu::Pause: rows = kPauseRows + kPauseExtraRows, x0 = 95, x1 = 430, above = 5, h = kPauseStep; break;
        case MouseMenu::Question: {
            std::array<AnswerBox, 2> a;
            Answers(a);
            for (int i = 0; i < 2; i++)
                if (x >= a[size_t(i)].x0 - 8 && x <= a[size_t(i)].x1 + 8 && y >= a[size_t(i)].y0 - 6 && y <= a[size_t(i)].y1 + 6)
                    return i;
            return -1;
        }
        default: return -1;
    }
    if (x < x0 || x > x1) return -1;
    for (int r = 0; r < rows; r++) {
        float t = top(r) - above;
        if (y >= t && y < t + h) return r;
    }
    return -1;
}

// the A or B prompt (0 A, 1 B) under (x, y): its picture, and its words to the right up to the next prompt
int PromptAt(float x, float y) {
    std::lock_guard l(g_prompt_lock);
    const FramePrompts* last = nullptr;
    for (const FramePrompts& fp : g_prompt_frames)
        if (fp.frame <= Frame() && Frame() - fp.frame <= 3 && (!last || fp.frame > last->frame)) last = &fp;
    if (!last) return -1;
    for (const PromptRect& p : last->list) {
        if ((p.prompt != Prompt::A && p.prompt != Prompt::B) || p.alpha < 0.5f) continue;
        float right = p.x1 + 110;
        for (const PromptRect& q : last->list)
            if (q.x0 > p.x1 && q.y0 < p.y1 && q.y1 > p.y0) right = std::min(right, q.x0 - 4);
        if (x >= p.x0 - 4 && x <= right && y >= p.y0 - 4 && y <= p.y1 + 4) return p.prompt == Prompt::A ? 0 : 1;
    }
    return -1;
}

// the pad, before PadInner reads it
void MouseMenus(input::Pad& pad) {
    static uint32_t moves_seen = 0, clicks_seen = 0;
    static MouseMenu menu = MouseMenu::None;
    static int target = -1, hold = 0, hold_button = 0;
    static bool accept = false;
    static int64_t at = 0;
    static uint64_t hold_until = 0, step_at = 0;  // frames: the game reads the pad once a frame, polled more often
    const uint32_t moves = g_cursor_moves.load(), clicks = g_cursor_clicks.load();
    const bool moved = moves != moves_seen;
    const bool clicked = clicks != clicks_seen && NowMs() - g_cursor_click_at.load() < 250;
    moves_seen = moves, clicks_seen = clicks;
    // only on a menu, with the cursor shown and no page up
    if (controls::CurrentGameState() == controls::GameState::Playing || g_page.load() != Page::None || !MouseClicks() ||
        overlay::Open()) {
        target = -1, accept = false, hold = 0;
        return;
    }
    // a button pressed for a click: held three frames, as a press (and no more than 30 polls)
    auto press = [&] {
        if (hold <= 0 || Frame() >= hold_until) {
            hold = 0;
            return false;
        }
        if (hold_button == 0) pad.analog[0] = 0xFF;
        else if (hold_button == 1) pad.analog[1] = 0xFF;
        else pad.buttons |= hold_button == 2 ? 4 : 8;
        hold--;
        return true;
    };
    auto start_press = [&](int button) {
        hold_button = button, hold = 30, hold_until = Frame() + 3;
        press();
    };
    if (press()) return;
    MouseMenu now = MouseMenu::None;
    int cur = MenuNow(now);
    const bool over = g_cursor_over.load();
    const float x = g_cursor_x.load(), y = g_cursor_y.load();
    int hover = over && now != MouseMenu::None ? MouseRowAt(now, x, y) : -1;
    if (clicked) {
        int prompt = over ? PromptAt(x, y) : -1;
        int side = over && now == MouseMenu::None ? SideAt(x, y) : -1;
        if (hover >= 0) {
            target = hover, accept = true, menu = now, at = NowMs();
        } else if (prompt >= 0 || now == MouseMenu::None) {
            // a prompt's button, the mission select's LEFT or RIGHT, or A on a screen without rows
            target = -1, accept = false;
            start_press(prompt >= 0 ? prompt : side >= 0 ? 2 + side : 0);
            return;
        }
    } else if (moved && hover >= 0 && hover != cur) {
        target = hover, accept = false, menu = now, at = NowMs();
    }
    if (target < 0) {
        step_at = 0;
        return;
    }
    // (a poll between the frames of a menu can find none up: then it waits)
    if (now == MouseMenu::None && NowMs() - at <= 2000) return;
    if (now != menu || cur < 0 || NowMs() - at > 2000) {
        target = -1, accept = false;
        return;
    }
    if (cur == target) {
        target = -1, step_at = 0;
        if (accept) accept = false, start_press(0);
        return;
    }
    // a press toward it, held two frames and let go three: the selection the menus' draws report has caught up by
    // the next one. The answers lie side by side: left and right.
    if (!step_at || Frame() >= step_at + 5) step_at = Frame();
    if (Frame() < step_at + 2) {
        if (now == MouseMenu::Question) {
            std::array<AnswerBox, 2> a;
            Answers(a);
            pad.buttons |= a[size_t(target)].x0 < a[size_t(cur)].x0 ? 4 : 8;
        } else {
            pad.buttons |= target > cur ? 2 : 1;
        }
    }
}

}  // namespace

void MouseCursor(float x, float y, bool over, bool moved, bool clicked) {
    g_cursor_x.store(x), g_cursor_y.store(y), g_cursor_over.store(over);
    if (moved) g_cursor_moves.fetch_add(1);
    if (clicked) g_cursor_click_at.store(NowMs()), g_cursor_clicks.fetch_add(1);
}

bool MouseClicks() {
    return (kMenus || kDrivingImage) && controls::MenuCursor() && g_page.load() == Page::None;
}

void DrivingQuestionWord(int word, bool lit) {
    std::lock_guard l(g_answer_lock);
    g_answer_open.store(word);
    if (word < 0 || word > 1) return;
    AnswerBox& a = g_answers[size_t(word)];
    if (a.seen != Frame() + 1) a = AnswerBox{}, a.seen = Frame() + 1;
    a.bright = lit ? 2.0f : 1.0f;
}

void Pad(int port, input::Pad& pad) {
    if (!(kMenus || kDrivingImage) || port != 0) return;
    MouseMenus(pad);
    // a multiplayer game's results (CONTINUE, X REMATCH) on the keyboard: Backspace (the menus' B) is X there
    if (kMenus && Keyboard() && Recent(g_rematch_seen.load())) {
        pad.analog[2] = std::max(pad.analog[2], pad.analog[1]);
        pad.analog[1] = 0;
    }
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
    // "Are you sure?" up (driving.xbe reports its rows' lit row as -1 then): the pad is the game's alone, left and right
    // between Yes and No as it has them, and nothing of this port's rows
    if (pause && (kDrivingImage ? lit < 0 : PauseQuestion())) {
        g_pause_extra.store(-1);
        return;
    }
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

// a grid's panel in the pause menu's look (below), for the pages' context
void PanelBackground(ImDrawList* dl, float x0, float y0, float x1, float y1);
// a scroll chevron of the pages (below)
void DrawChevron(ImDrawList* dl, float x, float y, bool up, ImU32 col);

namespace {

ImFont* g_font = nullptr;  // the game's lettering (GameFont), else Bahnschrift
bool g_game_font = false;
ImFont* g_title_font = nullptr;  // the italic lettering, for titles and tabs (else g_font)
// the same two at the game's own sheets' resolution (a texel of the sheet an atlas pixel, magnified with bilinear
// filtering as the game's own lettering is drawn): with the texture pack's menu part off, as the game's menus then are
ImFont* g_font_lo = nullptr;
ImFont* g_title_font_lo = nullptr;
bool g_body_outline = false;  // the body lettering's sheet has a dark outline round its letters (FillSheetFont)

// Menu And HUD Textures off (or the texture pack off): this port's menu parts at the game's own resolution too
bool LowMenus() { return !overlay::g_hd_menus || !app::Get().custom_textures; }
ImFont* BodyFont() {
    ImFont* f = LowMenus() && g_font_lo ? g_font_lo : g_font;
    return f ? f : ImGui::GetFont();
}
// the italic lettering's font, null without its sheet
ImFont* ItalicFont() { return LowMenus() && g_title_font_lo ? g_title_font_lo : g_title_font; }

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

// title: in the italic lettering (else the body's)
void SayIn(bool title, ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align) {
    ImFont* italic = title ? ItalicFont() : nullptr;
    ImFont* f = italic ? italic : BodyFont();
    float px = size * GuestScale();
    ImVec2 p = Guest(x, y);
    if (align != 0) p.x -= (italic ? ItalicLine(f, nullptr, px, p, col, text) : f->CalcTextSizeA(px, FLT_MAX, 0, text).x) * align;
    // no shadow copy: at PC resolutions its offset edge reads as a line along every letter
    if (italic) ItalicLine(f, dl, px, p, col, text);
    else dl->AddText(f, px, p, col, text);
}

void Say(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align = 0) {
    SayIn(false, dl, x, y, size, col, text, align);
}
void Title(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align) {
    SayIn(true, dl, x, y, size, col, text, align);
}
float WidthIn(bool title, float size, const char* text) {
    ImFont* italic = title ? ItalicFont() : nullptr;
    ImFont* f = italic ? italic : BodyFont();
    float px = size * GuestScale();
    if (italic) return ItalicLine(f, nullptr, px, ImVec2(0, 0), 0, text) / GuestScale();
    return f->CalcTextSizeA(px, FLT_MAX, 0, text).x / GuestScale();
}
float TitleWidth(float size, const char* text) { return WidthIn(true, size, text); }
float Width(float size, const char* text) { return WidthIn(false, size, text); }

bool g_layout_tab_set = false;  // Ui: the page reported its layout's tab this frame

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
    // on foot the grid (13 rows a side) ran into the curved column at the bottom left (it reaches x 128 there) and its
    // last row onto the Lives Remaining bar's top line (y 337 on the page's panel): a tenth smaller, between x 139
    // and 621, its last row ending near y 325
    if (!kDrivingImage) c.pause_grid_centre = 380, c.pause_grid_scale = 0.9f;
    c.panel = PanelBackground;
    c.chevron = DrawChevron;
    if (kDrivingImage) {
        c.game_back = [] { return Recent(g_layout_back_seen.load()); };
        c.back_x0 = kLayoutBackX - 3, c.back_x1 = kLayoutBackX + 45.4f + 11.7f + 3;
    }
    c.layout_tab = [](float x0, float x1) {
        g_layout_tab_x0.store(x0), g_layout_tab_x1.store(x1);
        g_layout_tab_on.store(true);
        g_layout_tab_set = true;
    };
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
    // the same letters a texel of the sheet an atlas pixel (Menu And HUD Textures off): FillSheetFont makes each from
    // its letter's pixels above, averaged over each texel
    ImFont* lo = nullptr;
    std::vector<int> lo_ids;
    int lo_space = -1;
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
    // the low resolution font: the same layout with k 1
    ImFontConfig lo_cfg;
    lo_cfg.SizePixels = float(mf.line_height);
    lo_cfg.GlyphRanges = io.Fonts->GetGlyphRangesDefault();
    std::snprintf(lo_cfg.Name, sizeof(lo_cfg.Name), "%s low", name);
    sf.lo = io.Fonts->AddFontDefault(&lo_cfg);
    for (int gi = 0; gi < mf.count; gi++) {
        const MenuGlyph& g = mf.glyphs[gi];
        sf.lo_ids.push_back(io.Fonts->AddCustomRectFontGlyph(sf.lo, ImWchar(static_cast<unsigned char>(g.c)), g.x1 - g.x0 + 3 + 4,
                                                             g.y1 - g.y0 + 3 + 4, float(g.advance), ImVec2(-3, float(g.dy + 1) - 2)));
    }
    sf.lo_space = io.Fonts->AddCustomRectFontGlyph(sf.lo, ImWchar(' '), 1, 1, float(mf.space), ImVec2(0, 0));
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
    auto put = [&](const ImFontAtlasCustomRect* r, const ImFontAtlasCustomRect* lr, const MenuGlyph* g) {
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
        // white over the dark outline, premultiplied (the low resolution copy averages these)
        std::vector<float> lit(cov.size()), alpha(cov.size());
        for (size_t i = 0; i < cov.size(); i++) {
            float c = cov[i], o = ring[i];
            alpha[i] = c + o * (1 - c), lit[i] = c;
        }
        auto write = [&](unsigned char* d, float c, float a) {
            // straight alpha: white over the dark outline (an outlined sheet's clear pixels dark too, so the
            // filtering does not lighten its edge)
            uint8_t v = a > 0.0f ? uint8_t(std::clamp(c / a * 255.0f + 0.5f, 0.0f, 255.0f)) : (sf.outline ? 0 : 255);
            d[0] = d[1] = d[2] = v;
            d[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                write(atlas + (size_t(r->Y + y) * aw + r->X + x) * 4, lit[size_t(y) * W + x], alpha[size_t(y) * W + x]);
        if (!lr) return;
        // the low resolution copy: each texel of the sheet under the glyph's rectangle and a texel round it the
        // average of the pixels above that cover it (they start at the sheet's pixel round((x0 - 1) k))
        int LW = lr->Width, LH = lr->Height;
        for (int ty = 0; ty < LH; ty++)
            for (int tx = 0; tx < LW; tx++) {
                float c = 0, a = 0;
                int cx = tx - kBorder, cy = ty - kBorder;
                if (g && cx >= 0 && cy >= 0 && cx < LW - 2 * kBorder && cy < LH - 2 * kBorder) {
                    float bx = std::round(float(g->x0 - 1) * sf.k), by = std::round(float(g->y0 - 1) * sf.k);
                    int px0 = int(std::lround(float(g->x0 - 1 + cx) * sf.k - bx)), px1 = int(std::lround(float(g->x0 + cx) * sf.k - bx));
                    int py0 = int(std::lround(float(g->y0 - 1 + cy) * sf.k - by)), py1 = int(std::lround(float(g->y0 + cy) * sf.k - by));
                    int n = 0;
                    for (int py = py0; py < py1; py++)
                        for (int px = px0; px < px1; px++) {
                            int x = px + kBorder, y = py + kBorder;
                            if (x < 0 || y < 0 || x >= W || y >= H) continue;
                            c += lit[size_t(y) * W + x], a += alpha[size_t(y) * W + x], n++;
                        }
                    if (n) c /= float(n), a /= float(n);
                }
                write(atlas + (size_t(lr->Y + ty) * aw + lr->X + tx) * 4, c, a);
            }
    };
    for (size_t i = 0; i < sf.ids.size(); i++)
        put(io.Fonts->GetCustomRectByIndex(sf.ids[i]), sf.lo ? io.Fonts->GetCustomRectByIndex(sf.lo_ids[i]) : nullptr,
            &sf.font->glyphs[i]);
    put(io.Fonts->GetCustomRectByIndex(sf.space), sf.lo ? io.Fonts->GetCustomRectByIndex(sf.lo_space) : nullptr, nullptr);
    // each glyph drawn from all but its rectangle's outer pixel (AddSheetFont): the filtering stays inside it
    float pu = 1.0f / float(aw), pv = 1.0f / float(ah);
    for (ImFont* font : {sf.im, sf.lo}) {
        if (!font) continue;
        for (int gi = 0; gi < sf.font->count; gi++) {
            auto* glyph = const_cast<ImFontGlyph*>(font->FindGlyphNoFallback(ImWchar(static_cast<unsigned char>(sf.font->glyphs[gi].c))));
            if (!glyph) continue;
            glyph->X0 += 1, glyph->Y0 += 1, glyph->X1 -= 1, glyph->Y1 -= 1;
            glyph->U0 += pu, glyph->V0 += pv, glyph->U1 -= pu, glyph->V1 -= pv;
        }
        // the space: nothing drawn (its 1x1 picture, drawn, took in its neighbours through the filtering)
        if (auto* sp = const_cast<ImFontGlyph*>(font->FindGlyphNoFallback(ImWchar(' ')))) sp->Visible = 0;
    }
    LOGI("menus: the pages use the game's lettering %s (%ux%u sheet%s)", sf.font->key, sf.w, sf.h, sf.outline ? ", outlined" : "");
}

// The keyboard's keys for the button prompts: pictures in the texture pack's folders (kb_enter.png and so on), put
// into ImGui's atlas beside the lettering.
struct KeyPicture {
    const wchar_t* file;
    std::vector<uint8_t> px;  // BGRA
    UINT w = 0, h = 0;
    int id = -1, lo_id = -1;  // the picture, and its low resolution copy (Menu & HUD textures off)
    float aspect = 1;
    ImVec2 uv0, uv1, lo_uv0, lo_uv1;
    bool ready = false;
};
KeyPicture g_keys[4] = {{L"kb_enter.png"}, {L"kb_backspace.png"}, {L"kb_updown.png"}, {L"kb_leftright.png"}};  // Prompt order
// the pause menu's moving light-blue background: the part of its 64x64 picture (pack key 4135a1a1b1d6547e, texels 1 to
// 63 across and 1 to 27 down) the game stretches over the panel twice, the second copy upside down, the two fading
// into each other (PanelBackground)
KeyPicture g_glow{L""};
// a scroll chevron of the pages (menus_pages.cpp Chevron) as a picture a texel a console pixel, for the low resolution
// look (Menu And HUD Textures off): its up form, 18 x 11 texels round its point (FillChevron)
KeyPicture g_chevron{L""};
constexpr int kChevronW = 18, kChevronH = 11;

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

std::filesystem::path FindSheet(const char* font_key);

void AddKeyPictures(ImGuiIO& io) {
    if (std::filesystem::path p = FindSheet("4135a1a1b1d6547e"); !p.empty() && LoadPicture(p, g_glow.px, g_glow.w, g_glow.h)) {
        // just the part the game draws, at the picture's own scale
        UINT k = g_glow.w / 64, w = 62 * k, h = 26 * k;
        // the >> marker drawn into the sheet's free part (MoveMarker): some of it opaque there
        size_t ink = 0;
        for (UINT y = UINT(kMarkerV0) * k; y < UINT(kMarkerV1 + 1) * k && y < g_glow.h; y++)
            for (UINT x = UINT(kMarkerU0) * k; x < UINT(kMarkerU1 + 1) * k && x < g_glow.w; x++) ink += g_glow.px[(size_t(y) * g_glow.w + x) * 4 + 3] > 200;
        g_marker_in_pack.store(ink >= size_t(20) * k * k);
        LOGI("menus: the pause marker %s", g_marker_in_pack.load() ? "from the texture pack's free part of its sheet" : "as the game draws it");
        std::vector<uint8_t> part(size_t(w) * h * 4);
        for (UINT y = 0; y < h; y++)
            std::memcpy(&part[size_t(y) * w * 4], &g_glow.px[(size_t(y + k) * g_glow.w + k) * 4], size_t(w) * 4);
        g_glow.px.swap(part), g_glow.w = w, g_glow.h = h;
        g_glow.id = io.Fonts->AddCustomRectRegular(int(w), int(h));
        g_glow.lo_id = io.Fonts->AddCustomRectRegular(62, 26);  // the game's own 62 x 26 texels (Menu And HUD off)
    }
    g_chevron.id = io.Fonts->AddCustomRectRegular(kChevronW, kChevronH);
    for (KeyPicture& k : g_keys) {
        std::filesystem::path p = FindFile(k.file);
        if (p.empty() || !LoadPicture(p, k.px, k.w, k.h)) continue;
        k.aspect = float(k.w) / float(k.h);
        // a transparent pixel round it, as the letters (FillKeyPictures)
        k.id = io.Fonts->AddCustomRectRegular(std::max(1, int(std::lround(kKeyHeight * k.aspect))) + 2, kKeyHeight + 2);
        k.lo_id = io.Fonts->AddCustomRectRegular(std::max(1, int(std::lround(kKeyHeight * k.aspect))) + 2, kKeyHeight + 2);
    }
}

// texels tall the low resolution keys have: about a texel a console pixel where the game's prompts are drawn, as the
// game's own pictures and lettering show with the texture pack's menu part off
constexpr int kKeyLowHeight = 12;

void FillKeyPictures(ImGuiIO& io) {
    unsigned char* atlas = nullptr;
    int aw = 0, ah = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    if (g_glow.id >= 0) {
        ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(g_glow.id);
        for (int y = 0; y < r->Height; y++)
            for (int x = 0; x < r->Width; x++) {
                const uint8_t* q = &g_glow.px[(size_t(y) * g_glow.w + x) * 4];
                unsigned char* d = atlas + (size_t(r->Y + y) * aw + r->X + x) * 4;
                d[0] = q[2], d[1] = q[1], d[2] = q[0], d[3] = q[3];
            }
        io.Fonts->CalcCustomRectUV(r, &g_glow.uv0, &g_glow.uv1);
        ImVec2 px((g_glow.uv1.x - g_glow.uv0.x) / r->Width, (g_glow.uv1.y - g_glow.uv0.y) / r->Height);
        g_glow.uv0.x += px.x * 0.5f, g_glow.uv0.y += px.y * 0.5f, g_glow.uv1.x -= px.x * 0.5f, g_glow.uv1.y -= px.y * 0.5f;
        // the low resolution copy: each of the game's texels the average of the picture's pixels over it
        if (g_glow.lo_id >= 0) {
            ImFontAtlasCustomRect* lr = io.Fonts->GetCustomRectByIndex(g_glow.lo_id);
            UINT k = std::max(1u, g_glow.w / 62);
            for (int y = 0; y < lr->Height; y++)
                for (int x = 0; x < lr->Width; x++) {
                    float acc[4] = {}, n = 0;
                    for (UINT sy = UINT(y) * k; sy < UINT(y + 1) * k && sy < g_glow.h; sy++)
                        for (UINT sx = UINT(x) * k; sx < UINT(x + 1) * k && sx < g_glow.w; sx++) {
                            const uint8_t* q = &g_glow.px[(size_t(sy) * g_glow.w + sx) * 4];
                            float a = q[3] / 255.0f;
                            acc[0] += q[2] * a, acc[1] += q[1] * a, acc[2] += q[0] * a, acc[3] += a;
                            n++;
                        }
                    unsigned char* d = atlas + (size_t(lr->Y + y) * aw + lr->X + x) * 4;
                    float a = n ? acc[3] / n : 0;
                    for (int c = 0; c < 3; c++) d[c] = uint8_t(a > 0 ? std::clamp(acc[c] / n / a, 0.0f, 255.0f) : 0);
                    d[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            io.Fonts->CalcCustomRectUV(lr, &g_glow.lo_uv0, &g_glow.lo_uv1);
            ImVec2 lp((g_glow.lo_uv1.x - g_glow.lo_uv0.x) / lr->Width, (g_glow.lo_uv1.y - g_glow.lo_uv0.y) / lr->Height);
            g_glow.lo_uv0.x += lp.x * 0.5f, g_glow.lo_uv0.y += lp.y * 0.5f, g_glow.lo_uv1.x -= lp.x * 0.5f, g_glow.lo_uv1.y -= lp.y * 0.5f;
        }
        g_glow.ready = true;
        g_glow.px.clear();
    }
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
        // the low resolution copy: box filtered down to kKeyLowHeight texels, then magnified back to the rectangle's
        // size with bilinear filtering, as a small texture is drawn
        if (k.lo_id >= 0) {
            ImFontAtlasCustomRect* lr = io.Fonts->GetCustomRectByIndex(k.lo_id);
            int lh = kKeyLowHeight, lw = std::max(1, int(std::lround(kKeyLowHeight * k.aspect)));
            std::vector<float> lo(size_t(lw) * lh * 4, 0.0f);  // premultiplied
            for (int y = 0; y < lh; y++)
                for (int x = 0; x < lw; x++) {
                    int sx0 = int(float(x) * k.w / lw), sx1 = std::max(sx0 + 1, int(float(x + 1) * k.w / lw));
                    int sy0 = int(float(y) * k.h / lh), sy1 = std::max(sy0 + 1, int(float(y + 1) * k.h / lh));
                    float acc[4] = {}, n = 0;
                    for (int sy = sy0; sy < sy1 && sy < int(k.h); sy++)
                        for (int sx = sx0; sx < sx1 && sx < int(k.w); sx++) {
                            const uint8_t* q = &k.px[(size_t(sy) * k.w + sx) * 4];
                            float a = q[3] / 255.0f;
                            acc[0] += q[2] * a, acc[1] += q[1] * a, acc[2] += q[0] * a, acc[3] += a;
                            n++;
                        }
                    for (int c = 0; c < 4; c++) lo[(size_t(y) * lw + x) * 4 + c] = n ? acc[c] / n : 0;
                }
            int iw = lr->Width - 2, ih = lr->Height - 2;
            auto at = [&](int x, int y, int c) {
                x = std::clamp(x, 0, lw - 1), y = std::clamp(y, 0, lh - 1);
                return lo[(size_t(y) * lw + x) * 4 + c];
            };
            for (int y = 0; y < lr->Height; y++)
                for (int x = 0; x < lr->Width; x++) {
                    unsigned char* d = atlas + (size_t(lr->Y + y) * aw + lr->X + x) * 4;
                    if (x == 0 || y == 0 || x == lr->Width - 1 || y == lr->Height - 1) {
                        d[0] = d[1] = d[2] = d[3] = 0;
                        continue;
                    }
                    float fx = (float(x - 1) + 0.5f) * lw / iw - 0.5f, fy = (float(y - 1) + 0.5f) * lh / ih - 0.5f;
                    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                    float ax = fx - x0, ay = fy - y0, v[4];
                    for (int c = 0; c < 4; c++)
                        v[c] = (at(x0, y0, c) * (1 - ax) + at(x0 + 1, y0, c) * ax) * (1 - ay) +
                               (at(x0, y0 + 1, c) * (1 - ax) + at(x0 + 1, y0 + 1, c) * ax) * ay;
                    float a = v[3];
                    for (int c = 0; c < 3; c++) d[c] = uint8_t(a > 0 ? std::clamp(v[c] / a, 0.0f, 255.0f) : 0);
                    d[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            io.Fonts->CalcCustomRectUV(lr, &k.lo_uv0, &k.lo_uv1);
            ImVec2 lp((k.lo_uv1.x - k.lo_uv0.x) / lr->Width, (k.lo_uv1.y - k.lo_uv0.y) / lr->Height);
            k.lo_uv0.x += lp.x, k.lo_uv0.y += lp.y, k.lo_uv1.x -= lp.x, k.lo_uv1.y -= lp.y;
        }
        k.ready = true;
        k.px.clear();
    }
}

// the up chevron's picture (g_chevron), after the fonts (its outline as the body lettering's): white where its stroke
// is, a dark outline round it when the lettering has one
void FillChevron(ImGuiIO& io) {
    if (g_chevron.id < 0) return;
    unsigned char* atlas = nullptr;
    int aw = 0, ah = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(g_chevron.id);
    constexpr float kHalfW = 6, kTall = 5.5f, kHalfStroke = 0.95f, kRing = 0.6f, kX = -9, kY = -2.75f;
    auto seg = [](float px, float py, float ax, float ay, float bx, float by) {
        float dx = bx - ax, dy = by - ay, t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
        float qx = ax + dx * t - px, qy = ay + dy * t - py;
        return std::sqrt(qx * qx + qy * qy);
    };
    for (int j = 0; j < r->Height; j++)
        for (int i = 0; i < r->Width; i++) {
            float c = 0, o = 0;
            for (int sj = 0; sj < 4; sj++)
                for (int si = 0; si < 4; si++) {
                    float px = kX + float(i) + (float(si) + 0.5f) / 4, py = kY + float(j) + (float(sj) + 0.5f) / 4;
                    float d = std::min(seg(px, py, -kHalfW, kTall, 0, 0), seg(px, py, 0, 0, kHalfW, kTall));
                    c += d <= kHalfStroke ? 1.0f / 16 : 0;
                    o += d <= kHalfStroke + kRing ? 1.0f / 16 : 0;
                }
            o = g_body_outline ? o * 0.85f : 0;
            float a = c + o * (1 - c);
            unsigned char* d = atlas + (size_t(r->Y + j) * aw + r->X + i) * 4;
            d[0] = d[1] = d[2] = uint8_t(a > 0 ? std::clamp(c / a * 255.0f + 0.5f, 0.0f, 255.0f) : 0);
            d[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    io.Fonts->CalcCustomRectUV(r, &g_chevron.uv0, &g_chevron.uv1);
    g_chevron.ready = true;
}

// a scroll chevron (more rows above: up, below: down) in the body lettering's look, centred on x, in the 11 px cell
// from y (console pixels): a chevron of the list's stroke weight (two console pixels at the rows' size), square-ended,
// with a thin dark outline when the lettering has one; with the menu part of the texture pack off a picture of it
// at the game's own resolution
void DrawChevron(ImDrawList* dl, float x, float y, bool up, ImU32 col) {
    constexpr float kHalfW = 6, kTall = 5.5f, kStroke = 1.9f, kRing = 0.6f;
    float top = y + (up ? 1.5f : 3.5f), bottom = top + kTall;
    if (LowMenus() && g_chevron.ready) {
        ImVec2 a = Guest(x - kChevronW / 2.0f, top - 2.75f), b = Guest(x + kChevronW / 2.0f, top - 2.75f + kChevronH);
        ImVec2 u0 = g_chevron.uv0, u1 = g_chevron.uv1;
        if (!up) std::swap(u0.y, u1.y);
        dl->AddImage(ImGui::GetIO().Fonts->TexID, a, b, u0, u1, col);
        return;
    }
    ImVec2 p[3] = {{x - kHalfW, up ? bottom : top}, {x, up ? top : bottom}, {x + kHalfW, up ? bottom : top}};
    if (g_body_outline) {
        // the outline: the stroke wider each side, its ends taken that much further out
        float len = std::sqrt(kHalfW * kHalfW + kTall * kTall), ex = kHalfW / len * kRing, ey = kTall / len * kRing;
        float sy = up ? 1.0f : -1.0f;
        ImVec2 o[3] = {Guest(p[0].x - ex, p[0].y + ey * sy), Guest(p[1].x, p[1].y), Guest(p[2].x + ex, p[2].y + ey * sy)};
        ImU32 dark = IM_COL32(0, 0, 0, int(float((col >> IM_COL32_A_SHIFT) & 0xFF) * 0.85f));
        dl->AddPolyline(o, 3, dark, ImDrawFlags_None, (kStroke + 2 * kRing) * GuestScale());
    }
    ImVec2 g[3] = {Guest(p[0].x, p[0].y), Guest(p[1].x, p[1].y), Guest(p[2].x, p[2].y)};
    dl->AddPolyline(g, 3, col, ImDrawFlags_None, kStroke * GuestScale());
}

// the moving background's picture in the atlas: the texture pack's, or at the game's own resolution with the menu
// part of the pack off
void GlowUV(ImVec2& u0, ImVec2& u1) {
    bool low = LowMenus() && g_glow.lo_id >= 0;
    u0 = low ? g_glow.lo_uv0 : g_glow.uv0, u1 = low ? g_glow.lo_uv1 : g_glow.uv1;
}

// a grid's panel in the pause menu's look: a dark base and the moving light-blue background over it (two copies of the
// picture, one upside down, fading into each other as the game's do), a plain dark panel without the picture
void PanelBackground(ImDrawList* dl, float x0, float y0, float x1, float y1) {
    ImVec2 a = Guest(x0, y0), b = Guest(x1, y1);
    dl->AddRectFilled(a, b, IM_COL32(4, 8, 24, g_glow.ready ? 170 : 225), 4 * GuestScale());
    if (!g_glow.ready) return;
    float t = float(double(NowMs() % 600000) / 1000.0);
    float f = 0.5f + 0.5f * std::sin(t * 0.9f);
    int a1 = int(255 * (0.25f + 0.5f * f)), a2 = int(255 * (0.75f - 0.5f * f));
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    ImVec2 u0, u1;
    GlowUV(u0, u1);
    dl->AddImage(tex, a, b, u0, u1, IM_COL32(255, 255, 255, a1));
    dl->AddImage(tex, a, b, ImVec2(u0.x, u1.y), ImVec2(u1.x, u0.y), IM_COL32(255, 255, 255, a2));
}

// driving.xbe's pause pages: the moving light-blue background the on-foot pages have (the same picture and fading as
// PanelBackground), under the page's words. The game's own (a 256x256 picture, 4a7ca93e) is barely there at its
// strength: on the page's grid it read as no background at all. Over the inside of the stretched panel only: from the
// tabs' bar (its bottom at y 97.6) down to the prompts' bar (its top at 376.2), its left side the slant the game's
// background has (x 60 at y 65.1 to x 100 at 380.2, inside the curve), over the place of the Lives Remaining / Mission
// Duration bar too (x 160 to 560 from y 334.4: on a page its box and top line are not drawn, DrivingPauseDraw).
void DrivingPageGlow(ImDrawList* dl) {
    if (!g_glow.ready) return;
    constexpr float kTop = 97.6f, kBottom = 380.2f, kEnd = 376.2f;
    constexpr float kRight = 640, kSlantY0 = 65.1f, kSlantX0 = 60, kSlantX1 = 100;
    auto left = [](float y) { return kSlantX0 + (kSlantX1 - kSlantX0) * (y - kSlantY0) / (kBottom - kSlantY0); };
    float t = float(double(NowMs() % 600000) / 1000.0);
    float f = 0.5f + 0.5f * std::sin(t * 0.9f);
    int a1 = int(255 * (0.25f + 0.5f * f)), a2 = int(255 * (0.75f - 0.5f * f));
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    ImVec2 u0, u1;
    GlowUV(u0, u1);
    // the picture over the whole area (x 60 to 640, y 97.6 to 380.2): each piece takes its part of it
    auto uv = [&](float x, float y, bool flip) {
        float u = (x - kSlantX0) / (kRight - kSlantX0), w = (y - kTop) / (kBottom - kTop);
        if (flip) w = 1 - w;
        return ImVec2(u0.x + (u1.x - u0.x) * u, u0.y + (u1.y - u0.y) * w);
    };
    auto quad = [&](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {  // clockwise from the top left
        for (int k = 0; k < 2; k++) {
            ImU32 col = IM_COL32(255, 255, 255, k ? a2 : a1);
            dl->AddImageQuad(tex, Guest(p0.x, p0.y), Guest(p1.x, p1.y), Guest(p2.x, p2.y), Guest(p3.x, p3.y), uv(p0.x, p0.y, k),
                             uv(p1.x, p1.y, k), uv(p2.x, p2.y, k), uv(p3.x, p3.y, k), col);
        }
    };
    quad({left(kTop), kTop}, {kRight, kTop}, {kRight, kEnd}, {left(kEnd), kEnd});
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
        float k = kDrivingImage || p.pause ? 1.0f : 0.88f;
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
        // with the texture pack's menu part off (or the pack off) the keys at the game's own prompts' resolution
        bool low = (!overlay::g_hd_menus || !app::Get().custom_textures) && key.lo_id >= 0;
        dl->AddImage(tex, Guest(cx - w / 2, cy - h / 2), Guest(cx + w / 2, cy + h / 2), low ? key.lo_uv0 : key.uv0,
                     low ? key.lo_uv1 : key.uv1, IM_COL32(255, 255, 255, alpha));
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
        have_keys |= g_glow.id >= 0;
        if (have_body || have_title || have_keys) {
            io.Fonts->Build();
            FillKeyPictures(io);
            if (have_body) FillSheetFont(io, body), g_font = body.im, g_font_lo = body.lo, g_game_font = true, g_body_outline = body.outline;
            if (have_title) FillSheetFont(io, title), g_title_font = title.im, g_title_font_lo = title.lo;
            FillChevron(io);
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

// The cursor for the game's own menus (MouseMenus, on the pad): where it is over the console's frame, whether it moved
// and whether the left button was clicked this frame. Not while ImGui wants the mouse (the F10 panel) or a page is up.
void FeedMouse() {
    ImGuiIO& io = ImGui::GetIO();
    float x = -1, y = -1;
    bool game = !io.WantCaptureMouse && g_page.load() == Page::None;
    bool over = game && MouseGuest(x, y);
    bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    MouseCursor(x, y, over, moved, game && ImGui::IsMouseClicked(0));
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
    ImFont* font = BodyFont();
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    // the font's cell puts a capital's top (dy + 2) texels below the text's top (AddSheetFont)
    ImVec2 at = Guest((p.x0 + p.x1) / 2, p.y0 - float(cap->dy + 2) * k);
    at.x -= extent.x / 2;
    ImU32 col = IM_COL32(int(std::clamp(p.color[0], 0.0f, 1.0f) * 255), int(std::clamp(p.color[1], 0.0f, 1.0f) * 255),
                         int(std::clamp(p.color[2], 0.0f, 1.0f) * 255), int(std::clamp(p.color[3], 0.0f, 1.0f) * 255));
    ImGui::GetBackgroundDrawList()->AddText(font, size, at, col, text);
}

void Ui() {
    if (kMenus) PressEnter();
    if (kMenus || kDrivingImage) DrawPrompts();
    if (!kMenus && !kDrivingImage) return;
    FeedMouse();
    Page page = g_page.load();
    if (page == Page::None) {
        // exactly on the frames the game draws its Controller Setup: not on the black screen after it
        if (kMenus && InShownFrame(g_setup_seen.load()) && SetupShows()) {
            controls::Context c = g_mission_query ? g_mission_query() : controls::Context::OnFoot;
            ControlsList(Context(), c, 130, 14);
        }
        return;
    }
    // a pause page: only over frames the game drew for it (its input kept meanwhile)
    if ((page == Page::PauseControls || page == Page::PausePcSettings) &&
        (!g_page_from.load() || app::g_shown_frame.load() + 1 < g_page_from.load()))
        return;
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
        // the rows not selected a good deal lighter than the game's own unlit colour: over the panel's bright moving
        // background that one was hard to read
        PageContext pc = Context();
        auto lighten = [](ImU32 col, float t) {
            ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
            v.x += (1 - v.x) * t, v.y += (1 - v.y) * t, v.z += (1 - v.z) * t;
            return ImGui::ColorConvertFloat4ToU32(v);
        };
        pc.unlit = lighten(pc.unlit, 0.45f), pc.value = lighten(pc.value, 0.45f);
        if (kDrivingImage) DrivingPageGlow(ImGui::GetForegroundDrawList());
        g_layout_tab_set = false;
        bool open = page == Page::PauseControls ? ControlsViewPage(pc, pn, controls::CurrentContext(), opened)
                                                : PausePcSettingsPage(pc, pn, controls::CurrentContext(), opened);
        if (!g_layout_tab_set || !open) g_layout_tab_on.store(false);
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
