#include "app/app.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>

// [frame_rate]: 60 (the port's loop on the wall clock, 60 game ticks a second) or 30 (the console's own fixed loop).
// The quick menu changes it through g_toggleFrameRate, taken at the loop's next pass (PaceWithoutSwap)
uint32_t g_frameRate = 60;
std::atomic<bool> g_toggleFrameRate{false};

namespace {
constexpr uint32_t kFastForward = 0x4A6E30;
bool g_render60 = false;
uint32_t g_lastSwap = 0;
float g_drawDelta = 1.0f / 30.0f;

void SetFrameRateMode(bool render60) {
    g_render60 = render60;
    const uint32_t flags = kernel::R32(0x4D9058);
    kernel::W32(0x4D9058, render60 ? flags & ~1u : flags | 1u);
}
}

extern std::atomic<bool> g_sixtyTicks;

bool FrameRateIs60() { return g_render60; }  // the mode the loop runs in now

void UseElapsedTime(X86Context&, uint8_t*) {
    const bool at60 = g_frameRate != 30;
    g_sixtyTicks.store(at60, std::memory_order_relaxed);
    app::Get().fps_cap = at60 ? 60 : 30;
    SetFrameRateMode(at60);
    g_lastSwap = 0;
}

// 60 game ticks: the camera's per-tick smoothing. The camera's apply (0x18BB80, the camera in ecx) blends its pose
// toward a target by factors (position +0x108, orientation +0x10C, field of view +0x44) and keeps the result as
// its next start: a factor t closes t of the gap each tick (the B zoom sets 0.4). Two 60 Hz ticks must close what
// one 30 Hz tick did, 1 - (1 - t)^2 = t30, so each apply uses 1 - sqrt(1 - t); the stored factors are put back after.
extern std::atomic<bool> g_sixtyTicks;
namespace {
constexpr uint32_t kCameraFactors[] = {0x44, 0x108, 0x10C};
uint32_t g_appliedCamera = 0;
uint32_t g_cameraFactors[3] = {};
}  // namespace
void BeginCameraApply(X86Context& ctx, uint8_t*) {
    g_appliedCamera = 0;
    if (!(g_render60 && g_sixtyTicks.load(std::memory_order_relaxed)) || !ctx.ecx) return;
    g_appliedCamera = ctx.ecx;
    for (int i = 0; i < 3; i++) {
        const uint32_t at = ctx.ecx + kCameraFactors[i];
        g_cameraFactors[i] = kernel::R32(at);
        const float t = std::bit_cast<float>(g_cameraFactors[i]);
        if (t > 0.0f && t < 1.0f) kernel::W32(at, std::bit_cast<uint32_t>(1.0f - std::sqrt(1.0f - t)));
    }
}
void EndCameraApply(X86Context&, uint8_t*) {
    if (!g_appliedCamera) return;
    for (int i = 0; i < 3; i++) kernel::W32(g_appliedCamera + kCameraFactors[i], g_cameraFactors[i]);
    g_appliedCamera = 0;
}

// 60 game ticks: the course intro's orbit around the flag (camera mode 0x1A14D0) turns the camera's offset from its
// centre by a fixed angle every tick ([0x4D9984], handed to the rotation at 0x183AB0). At 0x1A157A the angle is in
// eax on its way to the call; half of it each 60 Hz tick turns as far as all of it each 30 Hz tick. Only this call
// sees the change.
void HalveIntroOrbitStep(X86Context& ctx, uint8_t*) {
    if (g_render60 && g_sixtyTicks.load(std::memory_order_relaxed))
        ctx.eax = std::bit_cast<uint32_t>(std::bit_cast<float>(ctx.eax) * 0.5f);
}

// 60 game ticks: the aiming camera rig (0x58340) rebuilds the camera's direction every call and then tilts it by
// pitch x 0x378FE0 x step x 100 (0x58619 above the horizon, 0x5864B below). The tilt is not accumulated, so it is an
// angle that holds the 1/30 step the title always ran at: with a 1/60 step the camera stood a unit higher at the tee.
// Right after the multiply by the step (0x5861F, 0x58651) the value in xmm1 is scaled back to the 1/30 one.
void KeepAimTiltAt30(X86Context& ctx, uint8_t*) {
    if (g_render60 && g_sixtyTicks.load(std::memory_order_relaxed))
        ctx.xmm[1].f[0] *= (1.0f / 30.0f) / std::bit_cast<float>(kernel::R32(0x4D8FCC));
}

bool DrawBetweenSimulationTicks(X86Context& ctx, uint8_t*) {
    // At the accumulator check, xmm0 is elapsed time and xmm2 is the fixed step.
    // Enter the draw path directly when there is no simulation tick due.
    return g_render60 && ctx.xmm[0].f[0] < ctx.xmm[2].f[0] && kernel::R32(0x4D9008) != 0;
}

// 60 game ticks a second (Frame Rate 60, the default): native 60 FPS. The loop's fixed step is 0x4D8FCC, copied at
// start-up from the constant 0x2F594C (1/30), which nine other places also read directly; both become 1/60, and the
// accumulator restarts so the switch does not run a burst of ticks. Code that counts ticks instead of taking the
// step runs twice as fast; those found are fixed where they count (the camera eases, the intro orbit, the aim tilt,
// the golf cart and the beating minigame, below).
std::atomic<bool> g_sixtyTicks{false};
namespace {
constexpr uint32_t kStep = 0x4D8FCC, kStepConstant = 0x2F594C, kAccumulator = 0x4D8FD0;
void ApplyTickRate(X86Context& ctx) {
    const float want = g_render60 && g_sixtyTicks.load(std::memory_order_relaxed) ? 1.0f / 60.0f : 1.0f / 30.0f;
    if (std::bit_cast<float>(kernel::R32(kStep)) == want) return;
    kernel::W32(kStep, std::bit_cast<uint32_t>(want));
    kernel::W32(kStepConstant, std::bit_cast<uint32_t>(want));
    kernel::W32(kAccumulator, std::bit_cast<uint32_t>(want * 0.5f));  // half a tick in hand: see KeepTickPhase
    ctx.xmm[2].f[0] = want;  // the loop loaded the step for this iteration already
    LOGI("game ticks: %d a second", want < 0.02f ? 60 : 30);
}
// 60 game ticks drawn at 60 frames: each frame should run exactly one tick. The loop adds the time since its last pass
// to the accumulator and ticks while it holds a whole step, else draws. Drawing with the accumulator at 0 or a full
// step (where the switch used to leave it), the noise in the frame times decides between no tick and two: the
// animation stutters though the frames come evenly (the user saw it until a scene change's long frame moved it on).
// Each pass of the loop runs the ticks the accumulator holds, then draws; what is left after them should sit at half a
// step, the middle between the two. On each pass the accumulator is drawn a little (at most 0.1 ms) toward that: the
// frame times' noise then never crosses a tick, and the nudges come to nothing over time.
void KeepTickPhase(X86Context& ctx) {
    if (!g_render60 || !g_sixtyTicks.load(std::memory_order_relaxed) || kernel::R32(kFastForward)) return;
    const float step = ctx.xmm[2].f[0];
    if (step <= 0) return;
    float acc = std::bit_cast<float>(kernel::R32(kAccumulator));
    const float total = acc + ctx.xmm[1].f[0], left = total - std::floor(total / step) * step;
    acc += std::clamp((step * 0.5f - left) * 0.05f, -0.0001f, 0.0001f);
    kernel::W32(kAccumulator, std::bit_cast<uint32_t>(std::max(acc, 0.0f)));
}
}  // namespace

void UpdateFrameTiming(X86Context& ctx, uint8_t*) {
    ApplyTickRate(ctx);
    // ticks each drawn frame ran, a second at a time (60 ticks at 60 frames: all 1 when even)
    if (g_render60 && g_sixtyTicks.load(std::memory_order_relaxed)) {
        static uint32_t frames = 0, held[4] = {};
        const float step = ctx.xmm[2].f[0];
        const float total = std::bit_cast<float>(kernel::R32(kAccumulator)) + ctx.xmm[1].f[0];
        if (step > 0) held[std::min<uint32_t>(uint32_t(total / step), 3)]++;
        if (++frames == 60) {
            LOGI("game ticks per frame: 0 %u, 1 %u, 2 %u, 3+ %u", held[0], held[1], held[2], held[3]);
            frames = 0;
            held[0] = held[1] = held[2] = held[3] = 0;
        }
    }
    KeepTickPhase(ctx);
    g_drawDelta = ctx.xmm[1].f[0];
    // The fixed-frame loop gets 3x speed by drawing after every third tick.
    // With a wall clock, accrue three times as many unmodified 30 Hz ticks instead.
    if (g_render60 && kernel::R32(kFastForward)) ctx.xmm[1].f[0] *= 3.0f;
}

void UsePreviewDrawTime(X86Context& ctx, uint8_t*) {
    // The aiming preview scrolls its texture once per draw. Its other callers supply their own time step.
    if (g_render60) ctx.edx = std::bit_cast<uint32_t>(g_drawDelta);
}

void WrapTrajectoryTexture(X86Context& ctx, uint8_t*) {
    // The UV scroll is phase * 4/3. Wrap by 3/4 seconds to cross exactly one texture repeat.
    constexpr float originalPeriod = 0.714285671710968f;
    ctx.xmm[0].f[0] += 0.75f - originalPeriod;
}

void DrawDuringFastForward(X86Context& ctx, uint8_t*) {
    // Ignore the game's two-out-of-three draw skip in the wall-clock mode.
    if (g_render60 && kernel::R32(kFastForward)) ctx.eax = 0;
}

void PaceWithoutSwap(X86Context&, uint8_t*) {
    const uint32_t device = kernel::R32(0x268028);
    if (!device) return;
    if (g_toggleFrameRate.exchange(false, std::memory_order_relaxed)) {
        // Restart the elapsed-time clock at this boundary so time spent in fixed mode is not replayed.
        const uint64_t now = x86_rdtsc();
        kernel::W32(0x4F3988, uint32_t(now));
        kernel::W32(0x4F398C, uint32_t(now >> 32));
        kernel::W32(0x4D8FD0, std::bit_cast<uint32_t>(std::bit_cast<float>(kernel::R32(0x4D8FCC)) * 0.5f));
        app::Get().fps_cap = g_render60 ? 30 : 60;
        SetFrameRateMode(!g_render60);
    }
    const uint32_t swaps = kernel::R32(device + 0x2478);
    // D3DDevice_Swap already paces iterations that present a frame.
    if (g_render60 && swaps == g_lastSwap) app::Pace();
    g_lastSwap = swaps;
}

// InternalRelease has already decremented the parent texture's last reference.
// A header embedded in the course data has no heap block of its own to free.
// The shadow pass's render target texture loses an external reference about every tick until its count borrows from
// the type bits; once its counts reach zero it would be freed while the shadow pass still draws into it (a crash after
// a few minutes). A texture whose type bits no longer say texture keeps its memory.
bool SkipEmbeddedTextureFree(X86Context& ctx, uint8_t*) {
    const uint32_t texture = ctx.eax;
    const uint32_t common = kernel::R32(texture);
    if ((common & 0x70000) != 0x40000) {
        LOG_FIRST(4, "texture %08X with Common %08X kept: its reference count ran into its type bits", texture, common);
        return true;
    }
    return common == 0x00040000 && kernel::R16(texture - 16) == 0;
}

// Grass blades (0x3F070 writes each as a quad, 0x80 bytes) go into one of two vertex buffers the grass manager makes
// at its start (0x40CC0), 0x1FFF00 bytes each: 0x3FFE quads, a limit checked as each is added (0x3FBA0, 0x3FF70,
// 0x40350, 0x40720 keep it in esi, then compare it at the end of their loops). The triangles are drawn far to near
// (back to front, for blending), so with a long grass distance the faded far grass filled the buffer and the near grass
// was never drawn. The buffers are made larger (from the console's 64 MB contiguous memory, when enough of it is free
// at that point) and the limit raised to match.
uint32_t g_grassCap = 0x3FFE;  // quads

void GrassBufferSize(X86Context& ctx, uint8_t*) {
    static uint32_t scale = 0;
    if (!scale) {
        const mem::Stats st = mem::Query();
        const uint32_t free_mb = uint32_t((uint64_t(mem::kPhysicalSize / mem::kPage - st.contiguous_used) * mem::kPage) >> 20);
        scale = free_mb >= 40 ? 4 : free_mb >= 24 ? 2 : 1;
        g_grassCap = 0x3FFE * scale;
        LOGI("grass: %u MB of contiguous memory free, the grass buffers made %ux (%u quads, %u KB each)", free_mb, scale,
             g_grassCap, g_grassCap * 0x80 / 1024);
    }
    kernel::W32(ctx.esp, g_grassCap * 0x80);  // the size pushed for the buffer about to be made
}

void GrassBufferSizeKept(X86Context& ctx, uint8_t*) { kernel::W32(ctx.esi + 0x4D80FC, g_grassCap * 0x80); }

void GrassCapInEsi(X86Context& ctx, uint8_t*) { ctx.esi = g_grassCap; }

bool GrassRoomLeft(X86Context&, uint8_t*) { return kernel::R32(0x4661C0) < g_grassCap; }

// Grass distance (quick menu, [grass_distance] in percent). The grass manager (0x41090) gathers the ground's grass
// triangles within GrassDistance (0x330094, 80 on the disc) of the camera each frame and draws its tall grass over
// them, fading out towards that edge; it holds at most 0x800 triangles (the gather 0x3ED40 stops there). Right before
// the manager reads the distance, it is set to the disc's value scaled. The most triangles gathered is logged, to see
// how near the cap a longer distance runs.
uint32_t g_grassPercent = 100;

// A view that shows a lot of ground (the golf cart's) can need more blades than even the enlarged buffers hold at a
// long distance, and a full buffer loses the nearest grass. So the distance backs off on its own: within 5% of full,
// it comes in 10% a frame; under 80% it eases back out (about 2 seconds from half to whole). The far edge gives way,
// never the near grass, and never below the game's own distance
float g_grassBackoff = 1;

void ScaleGrassDistance(X86Context&, uint8_t*) {
    static float original = 0, written = 0;
    const float now = std::bit_cast<float>(kernel::R32(0x330094));
    if (now != written) original = now;  // the game's own (first call, or the game changed it)
    // the last frame's: ground triangles gathered (at most 0x800) and grass vertices built (at most 0x3FFE; they are
    // built far to near, so a full buffer loses the nearest grass)
    const uint32_t gathered = kernel::R32(0x4661BC), verts = kernel::R32(0x4661C0);
    const float percent = float(std::clamp<uint32_t>(g_grassPercent, 50, 600));
    const float floor = std::min(1.0f, 100.0f / percent), before = g_grassBackoff;
    if (verts >= g_grassCap - g_grassCap / 20) g_grassBackoff = std::max(floor, g_grassBackoff * 0.9f);
    else if (verts < g_grassCap - g_grassCap / 5) g_grassBackoff = std::min(1.0f, g_grassBackoff * 1.006f);
    if (before == 1 && g_grassBackoff < 1)
        LOGI("grass: %u of %u quads, the distance backs off to keep the near grass", verts, g_grassCap);
    if (before < 1 && g_grassBackoff == 1) LOGI("grass: the distance is back to %u%%", g_grassPercent);
    written = original * percent / 100.0f * g_grassBackoff;
    kernel::W32(0x330094, std::bit_cast<uint32_t>(written));
    static uint32_t most = 0, most_verts = 0, most_percent = 0;
    if (most_percent != g_grassPercent) most = most_verts = 0, most_percent = g_grassPercent;
    if (gathered > most + most / 8 || verts > most_verts + most_verts / 8 || (verts >= g_grassCap && most_verts < g_grassCap)) {
        most = std::max(most, gathered);
        most_verts = std::max(most_verts, verts);
        LOGI("grass: up to %u of %u triangles, %u of %u quads, at %u%% distance (%.0f), %u KB contiguous in use", most,
             0x800u, most_verts, g_grassCap, g_grassPercent, written, mem::Query().contiguous_used * 4);
    }
}

// 60 game ticks: the golf cart's physics. Its update (0x321A0) is handed the step and runs 8 sub-steps of an eighth
// of it (0x31A90, the eighth from 0x2F5528). On the console that is 240 sub-steps a second of 1/240; at 60 ticks it
// was 480 of 1/480, and what the physics applies once a sub-step (grip, the suspension's push, the torque) came twice
// as often: more gravity, more torque off the line, slippery handling. At 60 ticks each update runs 4 sub-steps of a
// quarter instead: the console's sub-steps, the same size and as many a second, and the cart still moves every frame
bool CartAtSixty() { return g_render60 && g_sixtyTicks.load(std::memory_order_relaxed); }
void CartSubStepSize(X86Context& ctx, uint8_t*) {  // 0x321B6: xmm0 the step times 1/8, about to be kept
    if (CartAtSixty()) ctx.xmm[0].f[0] *= 2.0f;
}
void CartSubStepCount(X86Context& ctx, uint8_t*) {  // 0x322EC: esi the sub-steps to run, 8
    if (CartAtSixty()) {
        ctx.esi = 4;
        LOG_FIRST(1, "cart: 4 physics sub-steps an update at 60 ticks, as the console's 8 at 30");
    }
}

// 60 game ticks: the camera that circles the golf cart when its minigame ends (camera mode 0x61110) turns its offset
// from the cart by a fixed 4 degrees every tick (pushed at 0x61252 for the rotation at 0x6126A; its countdown already
// takes the step). At 0x6125B the angle is on the stack; it is scaled by the step against the 1/30 it was made for
void ScaleCartOrbitStep(X86Context& ctx, uint8_t*) {
    if (!CartAtSixty()) return;
    const float angle = std::bit_cast<float>(kernel::R32(ctx.esp));
    const float step = std::bit_cast<float>(kernel::R32(0x4D8FCC));
    kernel::W32(ctx.esp, std::bit_cast<uint32_t>(angle * step * 30.0f));
}



// 60 game ticks: the beating minigame's prompt. The Beat Meter's sweep (0xCB9A0, each tick from its state 4) moves the
// prompt's place ([meter+0x40], between 35 and 420) by a fixed speed ([meter+0x6C]) a tick, down (0xCB9B8) or up
// (0xCB9D4), with no step: at 60 ticks it swept twice as fast. Right before each result is kept (0xCB9BD, 0xCB9D9)
// the move is scaled by the step against the 1/30 it was made for
namespace {
// xmm0 the place after the whole move, ahead (up) or back (down) of old by the speed
void ScaleBeatMove(X86Context& ctx, float dir) {
    if (!CartAtSixty()) return;
    const float speed = std::bit_cast<float>(kernel::R32(ctx.esi + 0x6C));
    const float k = std::bit_cast<float>(kernel::R32(0x4D8FCC)) * 30.0f;
    ctx.xmm[0].f[0] -= dir * speed * (1.0f - k);
}
}  // namespace
void ScaleBeatSweepDown(X86Context& ctx, uint8_t*) { ScaleBeatMove(ctx, -1.0f); }  // 0xCB9BD: old - speed
void ScaleBeatSweepUp(X86Context& ctx, uint8_t*) { ScaleBeatMove(ctx, 1.0f); }     // 0xCB9D9: old + speed
