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
#include <vector>

#include "common/overlay.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "image_symbols.h"

namespace menus {

namespace {

constexpr bool kMenus = std::string_view(IMAGE_XBE) == "bond_eng.xbe";

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// a screen counts as up while its rows were drawn in this frame or the one before (frames counted by the game's swaps)
uint64_t Frame() { return app::g_swaps.load(std::memory_order_relaxed); }
// seen holds the frame number plus one, 0 never
bool Recent(uint64_t seen) { return seen && Frame() + 1 >= seen && Frame() + 1 - seen <= 1; }

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
// for each of a letter's six vertices: right, bottom
std::array<std::array<bool, 2>, 6> g_corners{};
bool g_corners_known = false;

std::atomic<uint64_t> g_options_seen{0};
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
void Learn(const uint8_t* v, uint32_t stride, const Layout& l, uint32_t count, std::string_view text) {
    if (count != text.size() * 6) return;
    std::lock_guard lock(g_glyph_lock);
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
std::vector<uint8_t> Text(std::string_view text, const uint8_t* tmpl, uint32_t stride, const Layout& l, float x, float y,
                          const std::array<float, 4>& color) {
    std::lock_guard lock(g_glyph_lock);
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
// pages

enum class Page { None, Quit, PcSettings, Controls };
std::atomic<Page> g_page{Page::None};

enum class Nav { Up, Down, Left, Right, Accept, Back, PrevTab, NextTab };
std::mutex g_nav_lock;
std::deque<Nav> g_nav;

void Push(Nav n) {
    std::lock_guard lock(g_nav_lock);
    if (g_nav.size() < 32) g_nav.push_back(n);
}

void Open(Page p) {
    {
        std::lock_guard lock(g_nav_lock);
        g_nav.clear();
    }
    g_page.store(p);
}

// the pad's menu buttons, held
struct Held {
    bool up, down, left, right, a, b, lt, rt;
};

Held Read(const input::Pad& p) {
    constexpr int kStick = 16000;
    constexpr uint8_t kPressed = 0x80;
    Held h{};
    h.up = (p.buttons & 1) || p.ly > kStick;
    h.down = (p.buttons & 2) || p.ly < -kStick;
    h.left = (p.buttons & 4) || p.lx < -kStick;
    h.right = (p.buttons & 8) || p.lx > kStick;
    h.a = p.analog[0] > kPressed || (p.buttons & 0x10);  // START accepts too
    h.b = p.analog[1] > kPressed || (p.buttons & 0x20);  // and BACK goes back
    h.lt = p.analog[6] > kPressed || p.analog[5] > kPressed;
    h.rt = p.analog[7] > kPressed || p.analog[4] > kPressed;
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
    if (!kMenus) return;
    const auto& s = *static_cast<const gpu::State*>(state);
    Layout l = MenuLayout(s, prim, stride, count);
    if (!l.ok) return;
    Box b = BoxOf(v, stride, l, 0, count);

    if (count == 6) {
        // a main menu bar: its row moved up, and OPTIONS unlit while QUIT GAME is selected
        int band = BarBand(s, b);
        if (band >= 0) {
            int item = band % kMainItems;
            if (band < kMainItems) g_main_selected.store(item);
            g_main_seen.store(Frame() + 1);
            MoveY(v, stride, l, count, kNewTop + kNewStep * float(item) - kMainTops[item]);
            if (item == kMainItems - 1 && band < kMainItems && g_quit_selected.load()) SetBand(v, stride, l, count, band, band + kMainItems);
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
    if (!value && !shadow) {
        Learn(v, stride, l, count, kLabels[row]);
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
    std::vector<uint8_t> text = Text(kPcSettings, v, stride, l, first.x0, first.y0 + kInsert, color);
    if (!text.empty()) emit(prim, text.data(), uint32_t(text.size() / stride));
}

bool PageOpen() { return g_page.load() != Page::None; }

void PadInner(int port, input::Pad& pad, const Held& h);

void Pad(int port, input::Pad& pad) {
    if (!kMenus || port != 0) return;
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
    g_edges.last = h;

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

    // the main menu: QUIT GAME past OPTIONS
    bool main = Recent(g_main_seen.load());
    if (!main) g_quit_selected.store(false);
    if (main) {
        if (!g_quit_selected.load()) {
            if (dirs[1] && g_main_selected.load() == kMainItems - 1) {
                g_quit_selected.store(true);
                Take(pad, 1);
            }
        } else {
            if (dirs[0]) g_quit_selected.store(false);
            if (a) Open(Page::Quit);
            pad = Neutral(pad);
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
    }
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

#include <imgui.h>

#include "common/menus_pages.h"

namespace menus {

namespace {

ImFont* g_font = nullptr;  // Bahnschrift, the nearest a Windows install has to the menus' condensed sans

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
void Say(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* text, float align = 0) {
    ImFont* f = g_font ? g_font : ImGui::GetFont();
    float px = size * GuestScale();
    ImVec2 p = Guest(x, y);
    if (align != 0) p.x -= f->CalcTextSizeA(px, FLT_MAX, 0, text).x * align;
    float o = std::max(1.0f, GuestScale());
    dl->AddText(f, px, ImVec2(p.x + o, p.y + o), IM_COL32(0, 0, 0, 230), text);
    dl->AddText(f, px, p, col, text);
}

PageContext Context() {
    PageContext c;
    c.guest = Guest;
    c.scale = GuestScale;
    c.say = Say;
    c.lit = Rgb(g_lit_color);
    c.unlit = Rgb(g_unlit_color);
    c.value = Rgb(g_unlit_value_color);
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
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (!kMenus || g_page.load() == Page::None) return false;
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

void Ui() {
    if (!kMenus) return;
    Page page = g_page.load();
    if (page == Page::None) return;
    std::vector<Nav> nav = TakeNav();
    ImGui::GetIO().MouseDrawCursor = false;
    if (page == Page::Quit) {
        QuitPage(nav);
        return;
    }
    std::vector<PageNav> pn;
    for (Nav n : nav) pn.push_back(PageNav(int(n)));
    bool open = page == Page::PcSettings ? PcSettingsPage(Context(), pn) : ControlsPage(Context(), pn);
    if (!open) g_page.store(Page::None);
}

}  // namespace menus
#endif
