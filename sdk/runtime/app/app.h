#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height; 0 the console's own (classic)
    uint32_t render_height = 0;      // set: the picture is drawn this many lines high (1440, 2160...), past render_percent
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    uint32_t aa = 0;                 // anti-aliasing of the finished frame: 0 off, 1 FXAA, 2 SMAA
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

Settings& Get();
inline std::atomic<float> g_backdrop_blur{0};  // the frame blurred behind the quick menu, 0 to 1

// A picture of the game's own that the port's menu draws with. The renderer keeps a copy when the game first uploads
// a texture of this size whose bytes in guest memory hash to this (gpu::Hash); the game supplies the list before boot
// (g_ui_images), and id becomes the ImTextureID, bgra (rows packed) set before it
struct UiImage {
    uint64_t hash;
    uint32_t w, h;
    bool opaque = false;  // its alpha ignored (a texture the game draws without blending)
    std::atomic<void*> id{nullptr};
    std::vector<uint8_t> bgra;
};
inline constexpr int kUiImageSlots = 16;
inline UiImage* g_ui_images = nullptr;
inline int g_ui_image_count = 0;

void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the named picture heights offered above the output's own: 1440p, 4K, 5K, 8K
inline constexpr uint32_t kRenderHeights[] = {1440, 2160, 2880, 4320};
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();
// The vsync interval to present with: the monitor's refresh over the frame cap when it is a whole multiple of it
// (2 for 60 on 120 Hz, each frame held two refreshes), else 1. Kept by Pace.
uint32_t PresentInterval();

}  // namespace app
