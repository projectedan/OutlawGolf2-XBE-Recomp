#include "app/app.h"
#include "core/input.h"

#include <imgui.h>

#include <windows.h>
#include <atomic>
#include <iterator>

extern uint32_t g_frameRate;
extern uint32_t g_grassPercent;
extern bool g_bloom, g_focus;                     // quick_menu.cpp
extern uint32_t g_bloomPercent, g_focusPercent;

// quick_menu.cpp
void QuickMenuInit();
void QuickMenuFonts();
void QuickMenuUi();
bool QuickMenuKey(uint32_t msg, uintptr_t key, intptr_t lp);
void QuickMenuPad(int port, input::Pad& pad);
void QuickMenuPadConnected(int port, bool connected);

namespace {
constexpr unsigned kKeys[] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, 'C', 'N',
    'Z', 'X', 'F', 'R', 'E', 'Q', '1', '3',
    'A', 'D', 'S', 'W', 'J', 'L', 'K', 'I',
};
std::atomic<uint32_t> g_keys{0};

constexpr UINT_PTR kReleaseTimer = 0x4F4700;
constexpr uint64_t kMinPressMs = 65;
uint64_t g_pressedAt[std::size(kKeys)]{};  // window thread only

bool KeyboardMessage(void* window, uint32_t msg, uintptr_t key, intptr_t lp, intptr_t*) {
    auto hwnd = static_cast<HWND>(window);
    if (msg == WM_KILLFOCUS) {
        g_keys.store(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < std::size(kKeys); ++i) KillTimer(hwnd, kReleaseTimer + i);
    }
    if (msg == WM_TIMER && key >= kReleaseTimer && key < kReleaseTimer + std::size(kKeys)) {
        KillTimer(hwnd, key);
        g_keys.fetch_and(~(uint32_t{1} << (key - kReleaseTimer)), std::memory_order_relaxed);
        return true;
    }
    if (QuickMenuKey(msg, key, lp)) return true;  // F1, and every key while it is open
    if (msg != WM_KEYDOWN && msg != WM_KEYUP) return false;
    for (unsigned i = 0; i < std::size(kKeys); ++i) {
        if (key != kKeys[i]) continue;
        const uint32_t bit = uint32_t{1} << i;
        if (msg == WM_KEYDOWN) {
            if (!(lp & (1 << 30))) {
                KillTimer(hwnd, kReleaseTimer + i);
                g_pressedAt[i] = GetTickCount64();
                g_keys.fetch_or(bit, std::memory_order_relaxed);
            }
        } else {
            const uint64_t elapsed = GetTickCount64() - g_pressedAt[i];
            if (elapsed >= kMinPressMs || !SetTimer(hwnd, kReleaseTimer + i, UINT(kMinPressMs - elapsed), nullptr))
                g_keys.fetch_and(~bit, std::memory_order_relaxed);
        }
        return true;
    }
    return false;
}
void KeyboardPad(int port, input::Pad& pad) {
    if (port != 0) return;
    const uint32_t keys = g_keys.load(std::memory_order_relaxed);
    if (pad.connected && !keys) return;
    pad.connected = true;
    pad.buttons |= uint16_t(keys & 0xFF);
    for (unsigned i = 0; i < std::size(pad.analog); ++i)
        if (keys & (uint32_t{1} << (8 + i))) pad.analog[i] = 0xFF;
    auto axis = [keys](int negative, int positive, int16_t physical) -> int16_t {
        const bool low = (keys >> negative) & 1, high = (keys >> positive) & 1;
        if (low == high) return physical;
        return low ? -32768 : 32767;
    };
    pad.lx = axis(16, 17, pad.lx);
    pad.ly = axis(18, 19, pad.ly);
    pad.rx = axis(20, 21, pad.rx);
    pad.ry = axis(22, 23, pad.ry);
}
}  // namespace


// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Outlaw Golf 2",               // window and splash
    .short_title = L"OutlawGolf2",               // message boxes
    .name = L"OutlawGolf2",                      // window classes, crash dumps, handover event
    .log = "outlawgolf2",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "OutlawGolf2"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"frame_rate", nullptr, &g_frameRate}, {"grass_distance", nullptr, &g_grassPercent},
                {"bloom", &g_bloom, nullptr}, {"bloom_strength", nullptr, &g_bloomPercent},
                {"depth_of_field", &g_focus, nullptr}, {"depth_of_field_strength", nullptr, &g_focusPercent}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = QuickMenuInit,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = [](void*) { QuickMenuFonts(); },                   // the game window is up
    .message = KeyboardMessage,                   // window messages first; true takes one
    .ui = [](double) { QuickMenuUi(); },                     // ImGui drawing each frame
    .pad = [](int port, input::Pad& pad) {
        QuickMenuPadConnected(port, pad.connected);
        KeyboardPad(port, pad);
        QuickMenuPad(port, pad);
    },                      // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};
