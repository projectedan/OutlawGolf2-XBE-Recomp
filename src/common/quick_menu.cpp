// The port's quick menu, in the look of the game's own options screens: F1, or Start and Back together on a controller,
// opens and closes it. The game plays on behind it, blurred, without the controllers. Drawn by hand with ImGui's draw
// lists from the game's own pictures (its metal font, the blue background, the dimpled ball, the footer), which the
// renderer keeps when the front end first shows them (app::UiImage); plain shapes and Lato stand in until then.
// Steered like the game's menus: up and down a row, left and right its value, A to take, B to leave.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "gpu/backend.h"
#include "core/log.h"

extern uint32_t g_grassPercent;  // hooks.cpp
extern uint32_t g_frameRate;
extern std::atomic<bool> g_toggleFrameRate, g_sixtyTicks;
bool FrameRateIs60();

// [bloom], [depth_of_field] and their strengths in percent: the renderer's scene pass (gpu::g_scene_pass)
bool g_bloom = true, g_focus = true;
uint32_t g_bloomPercent = 50, g_focusPercent = 100;

void ApplyEffects() {
    g_bloomPercent = std::clamp<uint32_t>(g_bloomPercent, 10, 100);
    g_focusPercent = std::clamp<uint32_t>(g_focusPercent, 10, 100);
    gpu::g_bloom_glow = float(g_bloomPercent) / 100.0f * 1.6f;
    gpu::g_focus_blur = float(g_focusPercent) / 100.0f * 0.002f;  // the user: the first scale's 20% is the most
    gpu::g_scene_pass = (g_bloom ? 2 : 0) | (g_focus ? 4 : 0);
}

namespace {

// what a controller or the keyboard holds: the pad's own bits, then these
constexpr uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kStart = 0x10, kBack = 0x20, kA = 1 << 16, kB = 1 << 17,
                   kPrev = 1 << 18, kNext = 1 << 19;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
const unsigned char kLato[] = {
#embed "lato/Lato-Regular.ttf"
};
#pragma clang diagnostic pop

// the game's pictures, found by size and the hash of their bytes (the US disc's front end)
enum Picture { kFont, kFooter, kBlue, kBall, kChevron, kPictures };
app::UiImage g_pictures[kPictures] = {
    {0x68222F646CC9AC96ull, 256, 256},        // the large metal font
    {0xBFAE79A20C9CD670ull, 512, 128},        // the footer: a metal band, the green plate on its right
    {0x3E3D74240A4DAC1Dull, 256, 256, true},  // the blue scuffed background
    {0x1872F679EE4954D9ull, 512, 512},        // the dimpled ball
    {0x8A02C8A48B4A7F1Bull, 512, 32},         // the blue chevron strip
};

std::atomic<bool> g_open{false}, g_swallow{false}, g_fresh{false};  // swallow: closed, the game waits for every button up
std::atomic<bool> g_keyboard{false};                                  // the last input was the keyboard: its keys shown
std::atomic<uint32_t> g_pad[4], g_keys{0};
std::atomic<bool> g_connected[4];  // each port's pad, as last read: the boot hint names the buttons when one is in
ImFont* g_small = nullptr;

enum class Kind { Window, Resolution, VSync, FrameRate, Smoothing, Filtering, Grass, Bloom, BloomStrength, Focus, FocusStrength };
struct Row {
    Kind kind;
    const char* label;
    const char* about;  // the footer's line
};
constexpr int kPageCount = 3;
const std::vector<Row> kPages[kPageCount] = {
    {{Kind::Window, "WINDOW", "Fullscreen, borderless or in a window"},
     {Kind::Resolution, "RESOLUTION", "The console's own, your screen's, or sharper above it"},
     {Kind::VSync, "V-SYNC", "Waits for the screen, so the picture never tears"},
     {Kind::FrameRate, "FRAME RATE", "60, or the console's own 30"}},
    {{Kind::Smoothing, "ANTI-ALIASING", "Smooths jagged edges"},
     {Kind::Filtering, "TEXTURE FILTERING", "Keeps the ground sharp into the distance"},
     {Kind::Grass, "GRASS DISTANCE", "How far out the tall grass reaches"}},
    {{Kind::Bloom, "BLOOM", "A glow around the brightest parts of the picture"},
     {Kind::BloomStrength, "STRENGTH", "How strong the glow is"},
     {Kind::Focus, "DEPTH OF FIELD", "Softens the distance, as a camera's lens would"},
     {Kind::FocusStrength, "STRENGTH", "How soft the distance gets"}},
};
const char* const kPageNames[kPageCount] = {"DISPLAY", "GRAPHICS", "EFFECTS"};
constexpr size_t kMostRows = 4;
const char* const kModeNames[3] = {"Fullscreen", "Borderless", "Windowed"};
// the picture's size: the console's, the screen's, multiples of it. Taken with A, as each step remakes the targets
// Classic (the console's 480 lines), Desktop (the output's own), then the named heights above the output: each step
// costs the same on any screen, where a multiple of the output grew with it (3x was 4320 lines on a 1440p screen)
struct Scale {
    const char* name;
    uint32_t percent, height;
};
std::vector<Scale> Scales() {
    static const char* const kNames[] = {"1440p", "4K", "5K", "8K"};
    std::vector<Scale> out = {{"Classic", 0, 0}, {"Desktop", 100, 0}};
    const uint32_t screen = app::Output().height, now = app::Get().render_height;
    for (size_t i = 0; i < std::size(app::kRenderHeights); i++) {
        const uint32_t h = app::kRenderHeights[i];
        if (h > screen || h == now) out.push_back({kNames[i], 100, h});
    }
    return out;
}
const char* const kSmoothingNames[3] = {"Off", "FXAA", "SMAA"};
const char* const kFilterNames[5] = {"Off", "2x", "4x", "8x", "16x"};
constexpr uint32_t kFilterValues[5] = {0, 2, 4, 8, 16};
// 8x was dropped: it filled even the enlarged grass buffers (hooks.cpp), and the near grass blinked out
constexpr int kGrassSteps = 6;
const char* const kGrassNames[kGrassSteps] = {"Original", "1.5x", "2x", "3x", "4x", "6x"};
constexpr uint32_t kGrassPercent[kGrassSteps] = {100, 150, 200, 300, 400, 600};

int GrassNow() {
    int i = 0;
    while (i + 1 < kGrassSteps && g_grassPercent >= kGrassPercent[i + 1]) i++;
    return i;
}

// the game's colours
constexpr ImU32 kYellow = IM_COL32(255, 255, 80, 255), kWhite = IM_COL32(255, 255, 255, 255),
                kPale = IM_COL32(216, 220, 230, 255), kDim = IM_COL32(92, 96, 112, 255),
                kShade = IM_COL32(0, 0, 0, 200);

// the render thread's alone
struct State {
    int page = 0, row = 0;
    bool dirty = false;
    int pending = 0;                           // the scale on show, past the one in use while pending_on
    bool pending_on = false, leaving = false;  // leaving: Quit Game waits for A a second time
    uint32_t last = 0, repeat = 0;
    std::chrono::steady_clock::time_point held;
} g;

int ScaleNow() {
    const app::Settings& s = app::Get();
    if (s.render_percent == 0) return 0;
    const std::vector<Scale> scales = Scales();
    for (size_t i = 2; i < scales.size(); i++)
        if (scales[i].height == s.render_height) return int(i);
    return 1;
}

int FilteringNow() {
    uint32_t a = app::Get().aniso;
    return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
}

void Close() {
    if (!g_open.exchange(false)) return;
    g_swallow = true;
    g.pending_on = g.leaving = false;
    if (g.dirty) {
        app::Save();
        g.dirty = false;
    }
    LOGI("quick menu: closed");
}

void Open() {
    if (g_open.exchange(true)) return;
    g_fresh = true;
    LOGI("quick menu: open");
}

void Toggle() {
    if (g_open) Close();
    else Open();
}

// the rows, then Quit Game
void Move(int step) {
    int n = int(kPages[g.page].size()) + 1;
    g.row = (g.row + step + n) % n;
}

// a row's value one step on (-1, 1); confirm: A, not left or right
void Change(const Row& r, int step, bool confirm) {
    app::Settings& s = app::Get();
    switch (r.kind) {
        case Kind::Window:
            s.display_mode = app::DisplayMode((uint32_t(s.display_mode) + 3 + step) % 3);
            if (s.display_mode == app::DisplayMode::Borderless) s.width = s.height = 0;  // the desktop's size
            app::ApplyDisplay();
            break;
        case Kind::Resolution: {
            const std::vector<Scale> scales = Scales();
            const int n = int(scales.size());
            if (confirm) {
                if (!g.pending_on || g.pending >= n) return;
                s.render_percent = scales[size_t(g.pending)].percent;
                s.render_height = scales[size_t(g.pending)].height;
                g.pending_on = false;
                LOGI("quick menu: resolution %s", scales[size_t(g.pending)].name);
                break;
            }
            int now = g.pending_on ? g.pending : ScaleNow();
            g.pending = (now + step + n) % n;
            g.pending_on = g.pending != ScaleNow();
            return;
        }
        case Kind::VSync: s.vsync = !s.vsync; break;
        case Kind::FrameRate: {
            // 60 ticks with 60 frames, or neither; the loop switches over at its next pass
            g_frameRate = g_frameRate == 30 ? 60 : 30;
            const bool at60 = g_frameRate == 60;
            g_sixtyTicks.store(at60, std::memory_order_relaxed);
            if (at60 != FrameRateIs60()) g_toggleFrameRate.store(true, std::memory_order_relaxed);
            LOGI("quick menu: frame rate %u", g_frameRate);
            break;
        }
        case Kind::Smoothing:
            if (confirm) return;
            s.aa = (s.aa + 3 + step) % 3;
            break;
        case Kind::Filtering:
            if (confirm) return;
            s.aniso = kFilterValues[(FilteringNow() + step + 5) % 5];
            break;
        case Kind::Grass:
            if (confirm) return;
            g_grassPercent = kGrassPercent[(GrassNow() + step + kGrassSteps) % kGrassSteps];
            break;
        case Kind::Bloom: g_bloom = !g_bloom; ApplyEffects(); break;
        case Kind::Focus: g_focus = !g_focus; ApplyEffects(); break;
        case Kind::BloomStrength:
        case Kind::FocusStrength: {
            if (confirm) return;
            uint32_t& v = r.kind == Kind::BloomStrength ? g_bloomPercent : g_focusPercent;
            v = std::clamp<uint32_t>(v + uint32_t(step * 10), 10, 100);
            ApplyEffects();
            break;
        }
    }
    g.dirty = true;
}

void Input() {
    uint32_t now = g_keys.exchange(0), keys = now;
    for (auto& p : g_pad) now |= p.load();
    if (g_fresh.exchange(false)) g.last = now;
    uint32_t pressed = now & ~g.last;
    g.last = now & ~keys;  // a key's press comes once, the system repeats it
    // held up, down, left or right repeats
    auto t = std::chrono::steady_clock::now();
    uint32_t arrows = now & (kUp | kDown | kLeft | kRight);
    if (arrows != g.repeat) {
        g.repeat = arrows;
        g.held = t + std::chrono::milliseconds(350);
    } else if (arrows && t >= g.held) {
        g.held = t + std::chrono::milliseconds(90);
        pressed |= arrows;
    }
    if (pressed & (kB | kStart)) {
        if (g.leaving) g.leaving = false;
        else if (g.pending_on) g.pending_on = false;
        else if (!(now & kBack)) Close();
        return;
    }
    if (pressed & (kPrev | kNext)) {
        bool on_quit = g.row == int(kPages[g.page].size());
        g.page = (g.page + ((pressed & kNext) ? 1 : kPageCount - 1)) % kPageCount;
        int n = int(kPages[g.page].size());
        g.row = on_quit ? n : std::min(g.row, n - 1);
        g.pending_on = g.leaving = false;
    }
    const auto& rows = kPages[g.page];
    if (pressed & kUp) Move(-1);
    if (pressed & kDown) Move(1);
    if (pressed & (kUp | kDown)) g.pending_on = g.leaving = false;
    if (g.row == int(rows.size())) {
        if (pressed & kA) {
            if (g.leaving) {
                LOGI("quick menu: quit");
                Close();
                PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
                return;
            }
            g.leaving = true;
        }
        return;
    }
    const Row& r = rows[size_t(g.row)];
    if (pressed & kLeft) Change(r, -1, false);
    if (pressed & kRight) Change(r, 1, false);
    if (pressed & kA) Change(r, 1, true);
}

// ---- the game's font: its glyphs found by their alpha, row by row in the atlas's order ----

struct Glyph {
    float x = 0, y = 0, w = 0, h = 0;  // in the atlas's pixels
    bool set = false;
};
Glyph g_glyphs[128];
float g_cap = 32;  // a letter row's height in the atlas
bool g_font_ready = false;

void FindGlyphs() {
    const app::UiImage& f = g_pictures[kFont];
    if (g_font_ready || !f.id.load(std::memory_order_acquire)) return;
    const char* const kRows[] = {"012345", "6789+'!", "?@ABCDE", "FGHIJKL", "MNOPQR", "STUVWX", "YZ"};
    const int w = int(f.w), h = int(f.h);
    auto alpha = [&](int x, int y) { return f.bgra[(size_t(y) * w + x) * 4 + 3]; };
    std::vector<std::pair<int, int>> bands;
    for (int y = 0, start = -1; y <= h; y++) {
        bool any = false;
        for (int x = 0; y < h && x < w && !any; x++) any = alpha(x, y) > 40;
        if (any && start < 0) start = y;
        if (!any && start >= 0) {
            if (y - start > 8) bands.push_back({start, y});
            start = -1;
        }
    }
    for (size_t b = 0; b < bands.size() && b < std::size(kRows); b++) {
        auto [y0, y1] = bands[b];
        std::vector<std::pair<int, int>> runs;
        for (int x = 0, start = -1; x <= w; x++) {
            bool any = false;
            for (int y = y0; x < w && y < y1 && !any; y++) any = alpha(x, y) > 40;
            if (any && start < 0) start = x;
            if (!any && start >= 0) {
                runs.push_back({start, x});
                start = -1;
            }
        }
        // a glyph split by a clear column is joined again, at the narrowest gap, until the row's count is right
        const size_t want = std::strlen(kRows[b]);
        while (runs.size() > want) {
            size_t best = 0;
            int gap = INT32_MAX;
            for (size_t i = 0; i + 1 < runs.size(); i++)
                if (runs[i + 1].first - runs[i].second < gap) gap = runs[i + 1].first - runs[i].second, best = i;
            runs[best].second = runs[best + 1].second;
            runs.erase(runs.begin() + long(best) + 1);
        }
        for (size_t i = 0; i < runs.size() && i < want; i++)
            g_glyphs[uint8_t(kRows[b][i])] = {float(runs[i].first), float(y0), float(runs[i].second - runs[i].first),
                                              float(y1 - y0), true};
    }
    if (bands.size() > 2) g_cap = float(bands[2].second - bands[2].first);
    g_font_ready = bands.size() >= std::size(kRows);
    LOGI("quick menu: the game's font has %zu rows of glyphs%s", bands.size(), g_font_ready ? "" : "; Lato in its place");
    if (!g_font_ready) g_font_ready = true;  // looked once; Big falls back where a glyph is missing
}

// words in the game's metal font, h its letters' height, tinted (multiplied); align 0 from x, 0.5 about it, 1 to it.
// Returns the width; with no draw list it only measures
float Big(ImDrawList* dl, const char* s, float x, float y, float h, ImU32 tint = kWhite, float align = 0) {
    const app::UiImage& f = g_pictures[kFont];
    void* id = f.id.load(std::memory_order_acquire);
    const float k = h / g_cap, space = g_cap * 0.42f * k, track = 0.06f * h;
    auto advance = [&](char c) {
        const Glyph& gl = g_glyphs[uint8_t(c) & 127];
        return (gl.set ? gl.w : g_cap * 0.42f) * k + track;
    };
    bool whole = id;
    for (const char* c = s; *c && whole; c++) whole = *c == ' ' || *c == '-' || g_glyphs[uint8_t(*c) & 127].set;
    if (!whole) {  // Lato, until the game's font is kept
        float px = h * 1.3f;
        float w = g_small->CalcTextSizeA(px, FLT_MAX, 0, s).x;
        if (!dl) return w;
        x -= w * align;
        dl->AddText(g_small, px, ImVec2(x + 1.5f, y - h * 0.15f + 1.5f), kShade, s);
        dl->AddText(g_small, px, ImVec2(x, y - h * 0.15f), tint, s);
        dl->AddText(g_small, px, ImVec2(x + 0.7f, y - h * 0.15f), tint, s);
        return w;
    }
    float w = -track;
    for (const char* c = s; *c; c++) w += *c == ' ' || *c == '-' ? space + track : advance(*c);
    if (!dl) return w;
    x -= w * align;
    const ImVec2 size(float(f.w), float(f.h));
    for (const char* c = s; *c; c++) {
        if (*c == ' ') {
            x += space + track;
            continue;
        }
        if (*c == '-') {  // the font has none: a bar in its colours
            dl->AddRectFilled(ImVec2(x, y + h * 0.38f), ImVec2(x + space, y + h * 0.64f), IM_COL32(24, 24, 28, 255));
            dl->AddRectFilled(ImVec2(x + h * 0.05f, y + h * 0.43f), ImVec2(x + space - h * 0.05f, y + h * 0.57f),
                              (tint & 0xFF000000) | ((((tint & 0xFF) * 0xB8) / 255) | ((((tint >> 8) & 0xFF) * 0xBC / 255) << 8) |
                                                     ((((tint >> 16) & 0xFF) * 0xC4 / 255) << 16)));
            x += space + track;
            continue;
        }
        const Glyph& gl = g_glyphs[uint8_t(*c) & 127];
        dl->AddImage(id, ImVec2(x, y), ImVec2(x + gl.w * k, y + gl.h * k), ImVec2(gl.x / size.x, gl.y / size.y),
                     ImVec2((gl.x + gl.w) / size.x, (gl.y + gl.h) / size.y), tint);
        x += advance(*c);
    }
    return w;
}

// the game's plain small words: Lato in their place, with the game's dark edge
void Small(ImDrawList* dl, const char* s, float x, float y, float px, ImU32 col = kWhite, float align = 0) {
    float w = g_small->CalcTextSizeA(px, FLT_MAX, 0, s).x;
    x -= w * align;
    float o = std::max(1.0f, px * 0.07f);
    dl->AddText(g_small, px, ImVec2(x + o, y + o), kShade, s);
    dl->AddText(g_small, px, ImVec2(x, y), col, s);
    dl->AddText(g_small, px, ImVec2(x + px * 0.03f, y), col, s);  // a little heavier, as the game's
}

// the game's yellow arrow, dark edged; dir -1 left, 1 right
void Arrow(ImDrawList* dl, float x, float y, float s, float dir, ImU32 col = kYellow) {
    ImVec2 a(x + dir * s, y), b(x - dir * s * 0.6f, y - s * 0.75f), c(x - dir * s * 0.6f, y + s * 0.75f);
    dl->AddTriangleFilled(a, b, c, col);
    dl->AddTriangle(a, b, c, IM_COL32(16, 16, 16, 255), s * 0.25f);
}

void Button(ImDrawList* dl, float x, float y, float r, ImU32 light, ImU32 dark, const char* letter) {
    dl->AddCircleFilled(ImVec2(x, y), r * 1.15f, IM_COL32(24, 24, 24, 255), 24);
    dl->AddCircleFilled(ImVec2(x, y), r, dark, 24);
    dl->AddCircleFilled(ImVec2(x - r * 0.25f, y - r * 0.25f), r * 0.6f, light, 24);
    float px = r * 1.5f;
    ImVec2 sz = g_small->CalcTextSizeA(px, FLT_MAX, 0, letter);
    dl->AddText(g_small, px, ImVec2(x - sz.x / 2 + 0.5f, y - sz.y / 2), kWhite, letter);
    dl->AddText(g_small, px, ImVec2(x - sz.x / 2 + 1.0f, y - sz.y / 2), kWhite, letter);
}

void DrawPicture(ImDrawList* dl, Picture p, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
             ImU32 col = kWhite) {
    if (void* id = g_pictures[p].id.load(std::memory_order_acquire)) dl->AddImage(id, a, b, uv0, uv1, col);
}

// a row's bar as the game's options draw it: a small end cap, then the bar, lit when selected
void RowBar(ImDrawList* dl, float u, float x, float y, float w, float h, bool on) {
    ImU32 cap_top = on ? IM_COL32(236, 232, 120, 255) : IM_COL32(170, 174, 184, 255);
    ImU32 cap_bottom = on ? IM_COL32(120, 116, 40, 255) : IM_COL32(70, 72, 82, 255);
    dl->AddRectFilled(ImVec2(x - 0.6f * u, y - 0.6f * u), ImVec2(x + 4.6f * u, y + h + 0.6f * u), IM_COL32(12, 12, 14, 255), 1.2f * u);
    dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + 4 * u, y + h), cap_top, cap_top, cap_bottom, cap_bottom);
    float bx = x + 7 * u, bw = w - 7 * u;
    if (on) {
        float mid = bx + bw * 0.62f;
        ImU32 a = IM_COL32(138, 136, 48, 255), b = IM_COL32(104, 102, 34, 255), c = IM_COL32(30, 30, 10, 0);
        dl->AddRectFilledMultiColor(ImVec2(bx, y), ImVec2(mid, y + h), a, b, b, a);
        dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(bx + bw, y + h), b, c, c, b);
        dl->AddRectFilledMultiColor(ImVec2(bx, y), ImVec2(mid, y + h * 0.45f), IM_COL32(255, 255, 200, 40),
                                    IM_COL32(255, 255, 200, 20), IM_COL32(255, 255, 200, 0), IM_COL32(255, 255, 200, 0));
        dl->AddRect(ImVec2(bx, y), ImVec2(bx + bw, y + h), IM_COL32(20, 20, 10, 200), 0, 0, 0.8f * u);
    } else {
        dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + h), IM_COL32(6, 10, 24, 140));
        dl->AddRect(ImVec2(bx, y), ImVec2(bx + bw, y + h), IM_COL32(120, 126, 140, 140), 0, 0, 0.7f * u);
    }
}

// a thin steel line across, bright in the middle
void Steel(ImDrawList* dl, float left, float right, float y, float h, int alpha) {
    float mid = (left + right) / 2;
    ImU32 edge = IM_COL32(150, 160, 180, alpha * 3 / 10), centre = IM_COL32(205, 214, 228, alpha);
    dl->AddRectFilledMultiColor(ImVec2(left, y), ImVec2(mid, y + h), edge, centre, centre, edge);
    dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(right, y + h), centre, edge, edge, centre);
}

void Draw() {
    FindGlyphs();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float u = screen.y / 480.0f, cx = screen.x / 2;  // u: a console line
    const app::Settings& s = app::Get();

    // the box in the middle of the screen, the height of the longest page so that it holds still between them
    const float pw = 410 * u, left = cx - pw / 2, right = cx + pw / 2;
    const float head = 44 * u, row_h = 24 * u, body = 10 * u + kMostRows * row_h + 10 * u + 44 * u, foot = 52 * u;
    const float top = (screen.y - (head + body + foot)) / 2, fy = top + head + body, bottom = fy + foot;
    for (int i = 4; i > 0; i--) {
        float grow = float(i) * 4 * u;
        dl->AddRectFilled(ImVec2(left - grow, top - grow), ImVec2(right + grow, bottom + grow), IM_COL32(0, 0, 0, 40),
                          3 * u * float(i));
    }

    // the menus' background: the blue scuffed wall, the dimpled ball, darker to the left, the chevrons by the footer
    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, fy), IM_COL32(8, 18, 44, 255));
    dl->PushClipRect(ImVec2(left, top), ImVec2(right, fy), true);
    DrawPicture(dl, kBlue, ImVec2(left, top), ImVec2(right, fy), ImVec2(0, 0), ImVec2(1, 200.0f / 256.0f));
    DrawPicture(dl, kBall, ImVec2(left + pw * 0.18f, top - 40 * u), ImVec2(left + pw * 1.08f, top - 40 * u + pw * 0.9f),
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 140));
    dl->AddRectFilledMultiColor(ImVec2(left, top), ImVec2(cx, fy), IM_COL32(0, 0, 10, 140), IM_COL32(0, 0, 10, 40),
                                IM_COL32(0, 0, 10, 40), IM_COL32(0, 0, 10, 140));
    dl->AddRectFilledMultiColor(ImVec2(cx, top), ImVec2(right, fy), IM_COL32(0, 0, 10, 40), IM_COL32(0, 0, 10, 90),
                                IM_COL32(0, 0, 10, 90), IM_COL32(0, 0, 10, 40));
    DrawPicture(dl, kChevron, ImVec2(right - 150 * u, fy - 13 * u), ImVec2(right + 10 * u, fy - 3 * u), ImVec2(0, 0),
            ImVec2(1, 1), IM_COL32(255, 255, 255, 230));
    dl->PopClipRect();
    Steel(dl, left, right, top + 5 * u, 1.4f * u, 230);
    Steel(dl, left, right, top + 9 * u, 1.4f * u, 150);

    // the page: its name in the menus' yellow, arrows either side, the other page dim
    const float ty = top + 18 * u;
    Big(dl, kPageNames[g.page], cx, ty, 15 * u, kYellow, 0.5f);
    Arrow(dl, left + 22 * u, ty + 7.5f * u, 7 * u, -1);
    Arrow(dl, right - 22 * u, ty + 7.5f * u, 7 * u, 1);
    // the page before on the left, the page after on the right (with two pages, both were the other one)
    Big(dl, kPageNames[(g.page + kPageCount - 1) % kPageCount], left + 38 * u, ty + 3 * u, 9 * u, kDim, 0);
    Big(dl, kPageNames[(g.page + 1) % kPageCount], right - 38 * u, ty + 3 * u, 9 * u, kDim, 1);

    // the rows
    const auto& rows = kPages[g.page];
    const float rx = left + 24 * u, rw = pw - 48 * u, bar_h = row_h - 6 * u, vx = rx + rw * 0.72f;
    float y = top + head + 10 * u;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        bool on = int(i) == g.row;
        RowBar(dl, u, rx, y, rw, bar_h, on);
        Big(dl, r.label, rx + 13 * u, y + 3.5f * u, 11 * u, on ? kYellow : kWhite);
        const char* value = "";
        char percent[16];
        switch (r.kind) {
            case Kind::Window: value = kModeNames[uint32_t(s.display_mode) % 3]; break;
            case Kind::Resolution: {
                const std::vector<Scale> scales = Scales();
                const int i = g.pending_on ? g.pending : ScaleNow();
                value = i < int(scales.size()) ? scales[size_t(i)].name : "";
                break;
            }
            case Kind::VSync: value = s.vsync ? "On" : "Off"; break;
            case Kind::FrameRate: value = g_frameRate == 30 ? "30" : "60"; break;
            case Kind::Smoothing: value = kSmoothingNames[s.aa % 3]; break;
            case Kind::Filtering: value = kFilterNames[FilteringNow()]; break;
            case Kind::Grass: value = kGrassNames[GrassNow()]; break;
            case Kind::Bloom: value = g_bloom ? "On" : "Off"; break;
            case Kind::Focus: value = g_focus ? "On" : "Off"; break;
            case Kind::BloomStrength:
            case Kind::FocusStrength:
                std::snprintf(percent, sizeof(percent), "%u%%", r.kind == Kind::BloomStrength ? g_bloomPercent : g_focusPercent);
                value = percent;
                break;
        }
        bool waiting = r.kind == Kind::Resolution && g.pending_on;
        Small(dl, value, vx, y + 2.5f * u, 12.5f * u, waiting ? kYellow : kWhite, 0.5f);
        if (on) {
            Arrow(dl, vx - 44 * u, y + bar_h / 2, 5 * u, -1);
            Arrow(dl, vx + 44 * u, y + bar_h / 2, 5 * u, 1);
        }
        y += row_h;
    }

    // Quit Game, below a line, in the same place on every page
    y = top + head + 10 * u + kMostRows * row_h;
    dl->AddRectFilled(ImVec2(rx, y + 4 * u), ImVec2(rx + rw, y + 5 * u), IM_COL32(160, 170, 190, 90));
    y += 12 * u;
    bool quit = g.row == int(rows.size());
    RowBar(dl, u, rx, y, rw, bar_h, quit);
    Big(dl, g.leaving ? "PRESS A TO QUIT" : "QUIT GAME", cx + 3 * u, y + 3.5f * u, 11 * u, quit ? kYellow : kWhite, 0.5f);

    // the footer: the game's metal band with the green plate, the page's words, the buttons
    dl->AddRectFilled(ImVec2(left, fy), ImVec2(right, bottom), IM_COL32(2, 2, 6, 255));
    DrawPicture(dl, kFooter, ImVec2(left, fy), ImVec2(right, bottom), ImVec2(0, 0), ImVec2(1, 0.5f));
    // each line centred in its band: the metal (its middle 20.5 lines down, measured on screen) and the black strip
    // under it (32 to 52). The title fills about half the metal's height, as the game's own ("SELECT A GAME")
    constexpr float kTitle = 15;
    Big(dl, "QUICK MENU", left + 16 * u, fy + (20.5f - kTitle / 2) * u, kTitle * u);
    const char* about = quit ? (g.leaving ? "Press A again to close the game, B to stay" : "Close the game")
                        : g.pending_on && rows[size_t(g.row)].kind == Kind::Resolution ? "Press A to use this resolution"
                                                                                       : rows[size_t(g.row)].about;
    const float line = fy + 42 * u - g_small->CalcTextSizeA(10 * u, FLT_MAX, 0, "Ag").y / 2, by = fy + 42 * u;
    Small(dl, about, left + 16 * u, line, 10 * u, kPale);
    if (g_keyboard.load()) {
        Small(dl, "Enter  Accept     Esc  Back     Q / E  Page", right - 14 * u, line, 10 * u, kWhite, 1);
    } else {
        Small(dl, "LB / RB  Page", right - 128 * u, line, 10 * u, kWhite, 1);
        Button(dl, right - 118 * u, by, 5.5f * u, IM_COL32(123, 224, 112, 255), IM_COL32(31, 138, 28, 255), "A");
        Small(dl, "Accept", right - 109 * u, line, 10 * u);
        Button(dl, right - 58 * u, by, 5.5f * u, IM_COL32(240, 112, 112, 255), IM_COL32(160, 28, 28, 255), "B");
        Small(dl, "Back", right - 49 * u, line, 10 * u);
    }
}

// The boot hint: once a launch, a small bar at the top of the screen in the menu's look saying how to open the Quick
// Menu, with a controller's buttons when one is connected, else F1. In kHintDelay after the menu's pictures are there,
// up for kHintShow, faded at both ends; gone for good once the menu opens. The pictures come from the cache at once,
// but on a player's first run only when the game shows its menus (about 9 s in), and the hint waits for them so it
// never shows plain; past kHintLatest it shows with what there is
constexpr float kHintDelay = 1.5f, kHintShow = 6.0f, kHintFade = 0.5f, kHintLatest = 20.0f;
ImU32 Faded(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(float(c >> 24) * a) << 24); }

void Hint() {
    static float t = 0, waited = 0;
    static bool done = false;
    if (done || !g_small) return;
    if (g_open) {
        done = true;
        return;
    }
    const bool ready = g_pictures[kFont].id.load(std::memory_order_acquire) &&
                       g_pictures[kBlue].id.load(std::memory_order_acquire);
    if (!ready && waited < kHintLatest) {
        waited += ImGui::GetIO().DeltaTime;
        return;
    }
    if (t == 0) LOGI("quick menu: boot hint after %.1f s%s", waited, ready ? "" : ", without the game's pictures");
    t += ImGui::GetIO().DeltaTime;
    const float s = t - kHintDelay;
    if (s < 0) return;
    if (s > kHintShow) {
        done = true;
        return;
    }
    const float a = std::clamp(std::min(s, kHintShow - s) / kHintFade, 0.0f, 1.0f);
    FindGlyphs();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float u = screen.y / 480.0f, cx = screen.x / 2;
    bool pad = false;
    for (auto& c : g_connected) pad = pad || c.load();
    const char* const pre = "PRESS ";
    const char* const key = pad ? "START + BACK" : "F1";
    const char* const post = " FOR THE QUICK MENU";
    const float h = 10 * u;
    const float wp = Big(nullptr, pre, 0, 0, h), wk = Big(nullptr, key, 0, 0, h), wq = Big(nullptr, post, 0, 0, h);
    const float total = wp + wk + wq, bw = total + 40 * u, bh = 24 * u;
    const float left = cx - bw / 2, right = cx + bw / 2, top = 12 * u, bottom = top + bh;
    for (int i = 3; i > 0; i--) {
        float grow = float(i) * 3 * u;
        dl->AddRectFilled(ImVec2(left - grow, top - grow), ImVec2(right + grow, bottom + grow),
                          IM_COL32(0, 0, 0, int(40 * a)), 3 * u * float(i));
    }
    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(8, 18, 44, int(255 * a)));
    dl->PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);
    DrawPicture(dl, kBlue, ImVec2(left, top), ImVec2(right, bottom), ImVec2(0, 0.3f), ImVec2(1, 0.3f + bh / bw * 0.5f),
                Faded(kWhite, a));
    dl->AddRectFilledMultiColor(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(0, 0, 10, int(70 * a)),
                                IM_COL32(0, 0, 10, int(70 * a)), IM_COL32(0, 0, 10, int(140 * a)),
                                IM_COL32(0, 0, 10, int(140 * a)));
    dl->PopClipRect();
    Steel(dl, left, right, top + 2 * u, 1.2f * u, int(230 * a));
    Steel(dl, left, right, bottom - 3.2f * u, 1.2f * u, int(150 * a));
    float x = cx - total / 2;
    const float y = top + (bh - h) / 2;
    x += Big(dl, pre, x, y, h, Faded(kWhite, a));
    x += Big(dl, key, x, y, h, Faded(kYellow, a));
    Big(dl, post, x, y, h, Faded(kWhite, a));
}

}  // namespace

// what game_info.cpp hands the runtime

// before boot: the pictures the renderer keeps
void QuickMenuInit() {
    ApplyEffects();  // the settings file is read by now
    app::g_ui_images = g_pictures;
    app::g_ui_image_count = kPictures;
}

// the window is up, ImGui made: the small words' font
void QuickMenuFonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();  // the first font is the default: ImGui's own stays it
    ImFontConfig fc{};
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    g_small = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kLato), int(sizeof(kLato)), 40.0f, &fc);
}

bool QuickMenuOpen() { return g_open.load(); }

// the renderer's ImGui frame
void QuickMenuUi() {
    // the game blurred behind it, eased in and out over about a sixth of a second
    float blur = app::g_backdrop_blur.load(), want = g_open ? 1.0f : 0.0f;
    if (blur != want) {
        blur += (want - blur) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
        app::g_backdrop_blur = std::fabs(want - blur) < 0.01f ? want : blur;
    }
    Hint();
    if (!g_open) return;
    Input();
    if (g_open && g_small) Draw();
}

// window messages: F1 opens and closes it; open, it takes every key pressed (releases still reach the game's keys)
bool QuickMenuKey(uint32_t msg, uintptr_t key, intptr_t lp) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    if (key == VK_F1) {
        if (!(lp & (1 << 30))) {
            g_keyboard = true;
            Toggle();
        }
        return true;
    }
    if (!g_open) return false;
    g_keyboard = true;
    uint32_t bit = 0;
    switch (key) {
        case VK_UP: case 'W': bit = kUp; break;
        case VK_DOWN: case 'S': bit = kDown; break;
        case VK_LEFT: case 'A': bit = kLeft; break;
        case VK_RIGHT: case 'D': bit = kRight; break;
        case VK_RETURN: case VK_SPACE: bit = kA; break;
        case VK_ESCAPE: case VK_BACK: bit = kB; break;
        case 'Q': bit = kPrev; break;
        case 'E': bit = kNext; break;
        default: break;
    }
    g_keys |= bit;
    return true;
}

// whether a real controller is in a port, read before the keyboard is merged into port 0 (which marks it connected)
void QuickMenuPadConnected(int port, bool connected) {
    if (port >= 0 && port <= 3) g_connected[port] = connected;
}

// each pad as the game reads it: already polled, so nothing here waits on a controller
void QuickMenuPad(int port, input::Pad& pad) {
    if (port < 0 || port > 3) return;
    constexpr int kStick = 16000;
    uint32_t b = pad.buttons & 0x3F;
    if (pad.ly > kStick) b |= kUp;
    if (pad.ly < -kStick) b |= kDown;
    if (pad.lx < -kStick) b |= kLeft;
    if (pad.lx > kStick) b |= kRight;
    if (pad.analog[0] > 30) b |= kA;
    if (pad.analog[1] > 30) b |= kB;
    if (pad.analog[5] > 30) b |= kPrev;  // white (LB)
    if (pad.analog[4] > 30) b |= kNext;  // black (RB)
    if (b && b != g_pad[port].load() && g_open) g_keyboard = false;
    g_pad[port] = b;
    static std::atomic<bool> combo[4];
    bool both = (b & (kStart | kBack)) == (kStart | kBack);
    if (both && !combo[port].exchange(true)) {
        g_keyboard = false;
        Toggle();
    }
    if (!both) combo[port] = false;
    if (!g_open && g_swallow) {
        bool any = false;
        for (auto& p : g_pad) any = any || p.load();
        if (!any) g_swallow = false;
    }
    if (g_open || g_swallow) {
        bool connected = pad.connected;
        pad = input::Pad{};
        pad.connected = connected;
    }
}
