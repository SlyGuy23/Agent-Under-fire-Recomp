#include "gpu/figure.h"

#include <algorithm>
#include <cmath>

namespace gpu {

namespace {

constexpr int kS = int(kFigureScale);
constexpr int kMargin = 3;              // source texels kept round the figure: the filter's reach and the outline's
constexpr float kEdgeLo = 0.2f, kEdgeHi = 0.6f;  // upscaled coverage mapped to 0 and 1, smoothstepped between
constexpr int kOutline = 3;             // outline radius, in upscaled texels
constexpr float kOutlineAlpha = 0.45f;
constexpr float kOutlineBgr[3] = {0.90f, 0.65f, 0.55f};  // the pause screen frame's periwinkle

void CatmullRom(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

// kS samples per source texel: offsets of each from its texel's centre, as a base texel and four weights
struct Taps {
    int base[kS];
    float w[kS][4];
    Taps() {
        for (int k = 0; k < kS; k++) {
            float t = (k + 0.5f) / kS - 0.5f;
            float b = std::floor(t);
            base[k] = int(b);
            CatmullRom(t - b, w[k]);
        }
    }
};

}  // namespace

void SmoothFigure(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const int W = int(w), H = int(h), OW = W * kS, OH = H * kS;
    out.assign(size_t(OW) * OH * 4, 0);
    // the figure's box
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++) {
        const uint8_t* row = src + size_t(y) * pitch;
        for (int x = 0; x < W; x++) {
            if (row[x * 4 + 3]) {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) return;
    x0 = std::max(0, x0 - kMargin), y0 = std::max(0, y0 - kMargin);
    x1 = std::min(W - 1, x1 + kMargin), y1 = std::min(H - 1, y1 + kMargin);
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1, ow = bw * kS, oh = bh * kS;
    // premultiplied colour and coverage of the box, as floats
    std::vector<float> in(size_t(bw) * bh * 4);
    for (int y = 0; y < bh; y++) {
        const uint8_t* row = src + size_t(y0 + y) * pitch + size_t(x0) * 4;
        for (int x = 0; x < bw; x++) {
            float a = row[x * 4 + 3] / 255.0f;
            float* p = &in[(size_t(y) * bw + x) * 4];
            for (int c = 0; c < 3; c++) p[c] = row[x * 4 + c] / 255.0f * a;
            p[3] = a;
        }
    }
    static const Taps taps;
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    // across, then down
    std::vector<float> across(size_t(ow) * bh * 4);
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            for (int k = 0; k < kS; k++) {
                float acc[4] = {};
                for (int m = 0; m < 4; m++) {
                    int sx = clampi(x + taps.base[k] - 1 + m, 0, bw - 1);
                    const float* p = &in[(size_t(y) * bw + sx) * 4];
                    for (int c = 0; c < 4; c++) acc[c] += taps.w[k][m] * p[c];
                }
                std::copy(acc, acc + 4, &across[(size_t(y) * ow + x * kS + k) * 4]);
            }
        }
    }
    std::vector<float> up(size_t(ow) * oh * 4);
    for (int y = 0; y < bh; y++) {
        for (int k = 0; k < kS; k++) {
            float* dst = &up[size_t(y * kS + k) * ow * 4];
            for (int m = 0; m < 4; m++) {
                int sy = clampi(y + taps.base[k] - 1 + m, 0, bh - 1);
                const float* s = &across[size_t(sy) * ow * 4];
                float wgt = taps.w[k][m];
                for (int i = 0; i < ow * 4; i++) dst[i] += wgt * s[i];
            }
        }
    }
    // crisp coverage, then the outline as its dilation
    std::vector<float> cover(size_t(ow) * oh);
    for (size_t i = 0; i < cover.size(); i++) {
        float t = std::clamp((up[i * 4 + 3] - kEdgeLo) / (kEdgeHi - kEdgeLo), 0.0f, 1.0f);
        cover[i] = t * t * (3 - 2 * t);
    }
    std::vector<float> rowmax(cover.size()), grown(cover.size());
    // a disc as rows of a half width: the max over each row's span, then over the rows
    int half[2 * kOutline + 1];
    for (int d = -kOutline; d <= kOutline; d++) half[d + kOutline] = int(std::sqrt(float(kOutline * kOutline + 1 - d * d)));
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            float g = 0;
            for (int d = -kOutline; d <= kOutline; d++) {
                int yy = y + d;
                if (yy < 0 || yy >= oh) continue;
                int r = half[d + kOutline];
                const float* row = &cover[size_t(yy) * ow];
                for (int xx = std::max(0, x - r); xx <= std::min(ow - 1, x + r); xx++) g = std::max(g, row[xx]);
            }
            grown[size_t(y) * ow + x] = g;
        }
    }
    for (int y = 0; y < oh; y++) {
        uint8_t* dst = &out[(size_t(y0 * kS + y) * OW + size_t(x0) * kS) * 4];
        for (int x = 0; x < ow; x++) {
            size_t i = size_t(y) * ow + x;
            float a = cover[i], cov = std::max(up[i * 4 + 3], 1e-3f);
            float o = grown[i] * kOutlineAlpha * (1 - a);
            float oa = a + o;
            if (oa <= 0) continue;
            for (int c = 0; c < 3; c++) {
                float col = std::clamp(up[i * 4 + c] / cov, 0.0f, 1.0f);
                dst[x * 4 + c] = uint8_t(std::lround((col * a + kOutlineBgr[c] * o) / oa * 255));
            }
            dst[x * 4 + 3] = uint8_t(std::lround(std::min(oa, 1.0f) * 255));
        }
    }
}

}  // namespace gpu
