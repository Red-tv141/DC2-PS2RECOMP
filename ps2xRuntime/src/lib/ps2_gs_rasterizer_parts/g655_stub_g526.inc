// G655 P17: normal-build interfaces for the closed G526/G595 timing censuses.
static constexpr bool g_g526Draw = false;
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(
    void *Thread, unsigned long long *CycleTime);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);

enum G526Exit { kG526Rej = 0, kG526Cull, kG526Defer, kG526Inline, kG526ExitN };
enum G526Sub
{
    kG526Pro = 0, kG526Sw, kG526Build, kG526Dec, kG526Note,
    kG526Push, kG526Drain, kG526Edge, kG526Raster, kG526SubN
};
struct G526DrawScope { explicit G526DrawScope(uint32_t) {} };
static void g526SetExit(uint32_t) {}
template <typename... T> static void g526SetInlineKey(T &&...) {}
static void g526Lap(uint32_t) {}
static void g526Mark() {}

enum G595ProLap
{
    kG595Front = 0, kG595ZClr, kG595Pile, kG595Cull, kG595Body, kG595Elig, kG595ProN
};
static void g595ProBegin() {}
static void g595Lap(uint32_t) {}
