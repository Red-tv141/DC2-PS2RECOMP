// G793 -- frame-rate modes for DC2's 30 fps game logic (feature, default OFF).
//
//   DC2_FRAMERATE=30        (or unset / off) the game as shipped: nothing below is registered.
//   DC2_FRAMERATE=60        fixed 60 fps logic, every fix of SCUS-97213_1DF41F33.pnach "[60 Fps]" applied.
//   DC2_FRAMERATE=dynamic   up to 60 fps; each frame advances the game by the real time it took (1 or 2
//                           vblanks), so game speed stays correct anywhere between 60 and 30 fps.
//   DC2_FRAMERATE=72|120|144|240 (any 61..360)  EXPERIMENTAL high refresh: frames are paced by the host
//                           at that rate and the game advances by fractional 60 Hz ticks (see HIGH RATES).
//   DC2_FRAMERATE_MAX_STEP=<2..4>  largest step one frame may take, in 60 Hz ticks (default 2: speed is
//                           exact down to 30 fps and slows below it, like the original game).
//   DC2_FRAMERATE_CATCHUP_MS=<0..1000>  game time owed after a hitch longer than MAX_STEP is repaid over
//                           the next frames (default 500; 0 = drop it, the old behaviour).
//   DC2_FRAMERATE_STAIRS=stride|lift|pnach|off  stair step-up assist (default stride, see STAIRS).
//   DC2_FRAMERATE_NO=<list> disable single fixes, comma separated: anim blow pot weapon homing cutscene
//                           edges stun piyori gauge stairs texanim repeat ticks move.
//   DC2_FRAMERATE_TRACE=1   [G793:dfr] clock + [G793:hits] activity every 120 game frames.
//   DC2_FRAMERATE_STAIR_TRACE=1  [G793:stair] the player's MoveCheck in/out while moving.
//
// HOW THE GAME PACES ITSELF. mgFrameRate (gp-0x78A0 = 0x00376C50) is BOTH the number of vblanks
// mgEndFrame waits for AND the step that the frame-rate-aware systems multiply by
// (Human/Robo/MonsterMoveIF movement, sndStep(mgGetNowFrameRate()), the edit-mode cameras). Gameplay
// requests 2 (MainLoop, mgInit, TitleExit, InitRushMovie); the title and some menus request 1
// (TitleModeInit, SetMenuFrameRate). Everything else steps once per frame and was tuned for 30 fps.
//
// THE MODEL. While the game requests R = 2, a frame that spans T 60 Hz ticks writes mgFrameRate =
// max(1, floor(T)) and the pnach's fix list is made T-aware (T = 2 reproduces the original game, T = 1
// the pnach):
//   * due30 = "30 fps frames due" (acc += T) runs the systems the pnach toggled every other frame:
//     cutscene/event script, scene object and camera sequences, screen effects, Spheda power gauge,
//     the monster stun script, the dizzy-birds countdowns, texture animation.
//   * scale = T / R scales the per-frame constants the pnach halved: character motion step + blend
//     speed, scripted blow moves (leap slash), rocket/laser travel, homing turn rate, dizzy spin.
//   * k60 = "60 Hz ticks due" runs the per-tick systems that many times: thrown pots (the pnach's 60 Hz
//     constants), monster AI (CMonsterMan::ThinkHost) and effect scripts. Inside such a run every hook
//     sees a one-tick context, so a frame that took 2 ticks behaves like two 60 fps frames instead of
//     one 30 fps frame -- the same timing the fixed 60 mode has, at any frame rate.
// The pnach's code caves are replaced by call-site hooks: every wrapper chains to whatever was
// registered before it and is keyed on the guest return address where the callee is shared.
//
// WHY THE HOST CLOCK. Guest vblank handlers need the guest-execution lock, so while the EE is busy the
// interrupt worker dispatches ONE VSyncCallBack per wake however many periods passed (G377). The
// guest's vcount therefore under-counts exactly when a frame runs long -- which is why the pnach
// slows down below 60 fps. T comes from steady_clock, with the fraction carried to the next frame.
// A hitch longer than MAX_STEP is owed and repaid (CATCHUP_MS), so game time keeps up with real-time
// voice/BGM streams through a cutscene's loading spikes.
//
// HIGH RATES (> 60). mgEndFrame's vblank wait is skipped and the frame is paced on the host clock;
// raylib's present target follows (runtime_init_and_signals.inc). T is fractional. The world step
// (DngStep: monsters, effects, items, damage numbers, ...) runs on 60 Hz ticks, i.e. exactly as at 60
// fps; per frame run the player (movement, camera) and the draw path (character animation), scaled
// by T: the move vector each MoveIF hands to RockOn is scaled by T / mgFrameRate and the player's
// gravity is integrated per tick (CActionChara::RunScript). mgGetNowFrameRate returns T. Other game
// loops (sub-games, Georama, menus) are not ticked and may run faster than at 60.
//
// STAIRS. The first dungeon's stairs are a ramp ending in a lip. MoveCheck (0x14F5F0) snaps the player
// onto a higher floor only when this frame's target point already lies past the edge: at 30 fps one
// step reaches past the lip, at 60 fps the half step lands ON it, finds no floor, and the width test
// pushes the player back -- every frame (user recording route16-dungeon1-stairs60: x clamped at 3348,
// y ~ -6, measured in the live trace and reproduced by replay). `stride` (default): when a short step is
// blocked, the 30 fps-length step is tried too and kept only if it lands higher -- the step the
// original game takes. The pnach instead lifts the player 3.75 while moving (`lift`, or `pnach` with
// its absolute-address guards, one a heap word that is 0 in this port): measured on the recording it
// climbs only from some approach angles, and it turns every walk down a ramp into a fall.
//
// INPUT (pnach known issue 2). Event scripts read GetPadDown(GamePad) = cur & ~prev. Gated to every
// other frame, a script missed a press or a release that landed on a skipped frame ("the game thinks
// the button is still held"). While an event runs, pad edges are presented to the whole game on 30 fps
// frames only -- the original input cadence -- see presentEventInput.
namespace g793 {

using Fn = PS2Runtime::RecompiledFunction;

// ---- guest data (SCUS_972.13, gp = 0x0037E4F0) ----
constexpr uint32_t kMgFrameRate = 0x00376C50u; // gp-0x78A0  int   mgFrameRate
constexpr uint32_t kStopAnime = 0x00376C18u;   // gp-0x78D8  int   mgCTextureAnime::stop_anime
constexpr uint32_t kStairFlagA = 0x00376638u;  // pnach stair fix: skipped when != 0 (DngTreeMap byte)
constexpr uint32_t kStairFlagB = 0x00CF7B58u;  // pnach stair fix: skipped when == 0 (heap word)
constexpr uint32_t kGamePad = 0x003D76E0u;     // CGamePad GamePad: cur +0x04/+0x50, prev +0x9C/+0xE8
constexpr uint32_t kPadCtrl = 0x003D7B60u;     // CPadControl PadCtrl: Btn values at +0x10 + 8*i
constexpr uint32_t kPadCapture = kGamePad + 0x470u; // 2 = recorded-input playback (CapturePlay)
constexpr uint32_t kCharaMoveInfo = 0x910u;    // CActionChara: MoveCheckInfo (info[2] = grounded)

// ---- guest functions and call sites ----
constexpr uint32_t kFnEndFrame = 0x001425B0u;    // mgEndFrame__FP14mgCDrawManager
constexpr uint32_t kRaMainLoop = 0x001915E0u;    //   MainLoop__Fv (jal @0x1915D8)
constexpr uint32_t kFnWaitVSync = 0x001412A0u;   // WaitVSync__Fii
constexpr uint32_t kRaEndFrameWait = 0x00142A08u; //   mgEndFrame's frame wait
constexpr uint32_t kFnNowRate = 0x001421D0u;     // mgGetNowFrameRate__Fv
constexpr uint32_t kFnSetMenuRate = 0x00232CC0u; // SetMenuFrameRate__Fi
constexpr uint32_t kFnTitleInit = 0x002A1020u;   // TitleModeInit__Fv (writes 1)
constexpr uint32_t kFnLoadMovie = 0x002647B0u;   // LoadMovie__FPcP9mgCMemoryb
constexpr uint32_t kFnNormalDrive = 0x00174580u; // NormalDrive__11CCharacter2Fv (virtual)
constexpr uint32_t kFnScaleXYZ = 0x00107A58u;    // sceVu0ScaleVectorXYZ
constexpr uint32_t kRaBlowMove = 0x00171BC0u;    //   StepParam__12CActionCharaFv, blow move
constexpr uint32_t kFnScaleVec = 0x00107128u;    // sceVu0ScaleVector
constexpr uint32_t kRaRocketMove = 0x001B61D8u;  //   Step__15CRocketLauncherFv
constexpr uint32_t kRaLaserMove = 0x001B7238u;   //   Step__9CLaserGunFv
constexpr uint32_t kFnVecInterp = 0x00130A50u;   // mgVectorInterpolate__FPfPfPffi
constexpr uint32_t kRaRocketTurn = 0x001B61BCu;  //   Step__15CRocketLauncherFv
constexpr uint32_t kRaLaserTurn = 0x001B721Cu;   //   Step__9CLaserGunFv
constexpr uint32_t kFnPotThrow = 0x002CCEF0u;    // Throw__4CPotFv
constexpr uint32_t kFnPotFly = 0x002CC880u;      // FlyStep__4CPotFv
constexpr uint32_t kFnResume = 0x001871E0u;      // resume__10CRunScriptFv
constexpr uint32_t kRaEventLoop = 0x0025579Cu;   //   EventLoop__Fv (event / cutscene script)
constexpr uint32_t kRaMonsterRun = 0x001E0644u;  //   RunScript__11CMonsterManFi
constexpr uint32_t kFnObjSeq = 0x0025C710u;      // Play__12CSceneObjSeqFv
constexpr uint32_t kFnCmrSeq = 0x00259410u;      // Play__12CSceneCmrSeqFv
constexpr uint32_t kFnScreenEff = 0x00260630u;   // Step__13CScreenEffectFv
constexpr uint32_t kFnPiyori = 0x001C9B40u;      // Step__7CPiyoriFv
constexpr uint32_t kFnPowGage = 0x002E8F20u;     // Step__8CPowGageFv
constexpr uint32_t kFnRockOn = 0x0016C7A0u;      // RockOn__12CActionCharaFv (tail of every MoveIF)
constexpr uint32_t kRaHumanMove = 0x0016D530u;   //   HumanMoveIF__12CActionCharaFv
constexpr uint32_t kFnMoveCheck = 0x0014F5F0u;   // MoveCheck__FPfPfPfP13MoveCheckInfoP6CCPolyii
constexpr uint32_t kRaCharaMove = 0x00171574u;   //   RunScript__12CActionCharaF... (the player)
constexpr uint32_t kFnCharaScript = 0x00171210u; // RunScript__12CActionCharaFP6CSceneP14RUN_SCRIPT_ENV
constexpr uint32_t kFnTexAnime = 0x0013BD90u;    // TexAnime__15mgCTextureAnimeFiP13sceVif1Packet
constexpr uint32_t kFnPadUpdate = 0x0014A930u;   // UpDate__8CGamePadFv
constexpr uint32_t kRaMainLoopPad = 0x001914D0u; //   MainLoop's per-frame pad read (jal @0x1914C8)
constexpr uint32_t kFnPadCtrlUpd = 0x002ED550u;  // Update__11CPadControlFP8CGamePad
constexpr uint32_t kFnDngStep = 0x001D06C0u;     // DngStep__Fv (the dungeon / town world step)
constexpr uint32_t kFnThinkHost = 0x001DFB00u;   // ThinkHost__11CMonsterManFv (monster AI + motion)
constexpr uint32_t kFnEffectStep = 0x002E1600u;  // Step__16CEffectScriptManFv

constexpr uint32_t kMonsterStunScript = 500u; // RunScript__11CMonsterManFi: active script id +0x115A
constexpr float kStairLift = 3.75f;           // the pnach's lift (lui 0x4070)

enum Mode : uint32_t { ModeOff = 0, Mode60 = 1, ModeDynamic = 2, ModeHigh = 3 };
enum Stairs : uint32_t { StairsOff = 0, StairsStride = 1, StairsLift = 2, StairsPnach = 3 };

enum Fix : uint32_t
{
    FAnim = 1u << 0,     // NormalDrive motion step + blend speed            (pnach 0x174698/0x1746E0)
    FBlow = 1u << 1,     // scripted blow move, leap slash                   (pnach 0x3560C8)
    FPot = 1u << 2,      // thrown pot physics                               (pnach 0x2CC8D8..0x2CCFD4)
    FWeapon = 1u << 3,   // rocket launcher / laser gun travel               (pnach 0x1B61C8/0x1B7228)
    FHoming = 1u << 4,   // rocket / laser homing turn rate                  (new)
    FCutscene = 1u << 5, // event script, object + camera sequences, screen effects (pnach caves)
    FEdges = 1u << 6,    // input edges for the gated event script           (pnach Spheda input hack)
    FStun = 1u << 7,     // monster stun script 500                          (pnach 0x1E063C)
    FPiyori = 1u << 8,   // dizzy birds countdowns + spin                    (pnach 0x1C9Bxx)
    FGauge = 1u << 9,    // Spheda power gauge                               (pnach 0x2EA06C)
    FStairs = 1u << 10,  // stair step-up assist                             (pnach 0x16D520)
    FTexAnime = 1u << 11, // texture animation speed                         (pnach known issue 1)
    FRepeat = 1u << 12,  // pad auto-repeat timing                           (new)
    FTicks = 1u << 13,   // monster AI + effect scripts per 60 Hz tick       (new)
    FMove = 1u << 14,    // high rates: fractional player movement + gravity (new)
    FAll = (1u << 15) - 1u,
};

struct FixName
{
    const char *name;
    uint32_t bit;
};
constexpr FixName kFixNames[] = {
    {"anim", FAnim},       {"blow", FBlow},         {"pot", FPot},       {"weapon", FWeapon},
    {"homing", FHoming},   {"cutscene", FCutscene}, {"edges", FEdges},   {"stun", FStun},
    {"piyori", FPiyori},   {"gauge", FGauge},       {"stairs", FStairs}, {"texanim", FTexAnime},
    {"repeat", FRepeat},   {"ticks", FTicks},       {"move", FMove},
};

enum Site : uint32_t
{
    SEndFrame, SWaitVSync, SNowRate, SSetMenuRate, STitleInit, SLoadMovie, SNormalDrive, SScaleXYZ,
    SScaleVec, SVecInterp, SPotThrow, SPotFly, SResume, SObjSeq, SCmrSeq, SScreenEff, SPiyori, SPowGage,
    SRockOn, SMoveCheck, SCharaScript, STexAnime, SPadUpdate, SDngStep, SThinkHost, SEffectStep, SCount
};
inline Fn prev[SCount]{};

// The step one stretch of game logic runs at. `frame` is this rendered frame; `tick` replaces it while
// a per-tick system (DngStep at high rates, ThinkHost, effect scripts) runs one of its 60 Hz ticks.
struct Ctx
{
    float ticks = 2.0f;     // T: 60 Hz ticks covered
    uint32_t intRate = 2u;  // the mgFrameRate the guest sees
    uint32_t due30 = 1u;    // 30 fps frames due
    uint32_t k60 = 1u;      // 60 Hz ticks due
    float scale = 1.0f;     // T / R
    float moveScale = 1.0f; // T / intRate: what an mgFrameRate-scaled move still lacks
};

constexpr uint32_t kMaxTicks = 8u;

struct State
{
    uint32_t mode = ModeOff;
    uint32_t fixes = 0u;
    uint32_t cap = 2u;
    uint32_t hostFps = 0u; // > 60: frames are paced on the host clock (ModeHigh)
    uint32_t stairs = StairsStride;
    double catchupUs = 500000.0;
    bool trace = false;
    bool stairTrace = false;
    double periodUs = 16667.0;

    // The rate the game asked for, and the last value this file wrote over it.
    uint32_t requested = 2u;
    uint32_t lastWritten = 0xFFFFFFFFu;
    bool on = false; // requested == 2 and the mode is live for the current frame

    Ctx frame{};
    Ctx tick{};
    bool inTick = false;
    uint32_t tickDue[kMaxTicks]{}; // due30 of each of this frame's 60 Hz ticks

    double acc30 = 0.0, acc60 = 0.0;
    uint32_t tickAcc = 0u;
    double bankUs = 0.0; // fraction of a tick carried to the next frame
    double owedUs = 0.0; // game time a hitch could not pay (DC2_FRAMERATE_CATCHUP_MS)
    std::chrono::steady_clock::time_point lastEnd{};    // last clock tick (MainLoop's pad read)
    std::chrono::steady_clock::time_point lastPaced{};  // last MainLoop frame end (host pacing)
    bool havePaced = false;
    bool haveLast = false;
    bool discontinuity = true;
    uint32_t sideFrames = 0u;
    uint64_t frameNo = 0u; // MainLoop frames

    // event input, presented on 30 fps frames (presentEventInput)
    bool eventValid = false;
    uint64_t eventFrame = 0u;
    bool presentValid = false;
    uint32_t base0 = 0u, base1 = 0u;

    // thrown pots in 60 Hz ticks: guest address -> ticks flown
    std::unordered_map<uint32_t, uint32_t> pots;

    // the player's per-tick gravity (RunScript, high rates)
    uint32_t player = 0u;
    bool vyValid = false;
    float vyRaw = 0.0f, vyScaled = 0.0f;

    // trace window
    uint32_t winFrames = 0u, winTwo = 0u, winDue = 0u, winK60 = 0u;
    double winTicks = 0.0, winUs = 0.0, winOwed = 0.0;
    uint32_t stairLines = 0u;
};
inline State st;

inline const Ctx &cur() { return st.inTick ? st.tick : st.frame; }
inline bool fix(uint32_t bit) { return (st.fixes & bit) != 0u; }

// Per-fix activity, cumulative, printed by DC2_FRAMERATE_TRACE: proof that each hook is reached AND
// changed something on the route being tested (an inert hook prints 0).
enum Hit : uint32_t
{
    HAnim, HBlow, HWeapon, HHoming, HPotThrow, HPotTick, HEventRun, HEventSkip, HEdgeSwap, HStun,
    HSeqSkip, HCmrSkip, HPiyori, HGauge, HStairs, HStairFallback, HTexStop, HRepeat, HSide, HTickRun,
    HTickSkip, HMove, HGravity, HCount
};
inline uint64_t hits[HCount]{};
constexpr const char *kHitNames[HCount] = {
    "anim", "blow", "weapon", "homing", "potThrow", "potTick", "eventRun", "eventSkip", "edgeSwap",
    "stun", "seqSkip", "cmrSkip", "piyori", "gauge", "stairs", "stairFallback", "texStop", "repeat",
    "sideFrames", "tickRun", "tickSkip", "move", "gravity"};

// ---- guest memory ----
inline uint32_t rd(uint8_t *rdram, uint32_t a)
{
    const uint8_t *p = getConstMemPtr(rdram, a);
    uint32_t v = 0u;
    if (p)
        std::memcpy(&v, p, 4);
    return v;
}
inline void wr(uint8_t *rdram, uint32_t a, uint32_t v)
{
    if (uint8_t *p = getMemPtr(rdram, a))
        std::memcpy(p, &v, 4);
}
inline uint16_t rd16(uint8_t *rdram, uint32_t a)
{
    const uint8_t *p = getConstMemPtr(rdram, a);
    uint16_t v = 0u;
    if (p)
        std::memcpy(&v, p, 2);
    return v;
}
inline void wr16(uint8_t *rdram, uint32_t a, uint16_t v)
{
    if (uint8_t *p = getMemPtr(rdram, a))
        std::memcpy(p, &v, 2);
}
inline float rdf(uint8_t *rdram, uint32_t a)
{
    const uint32_t b = rd(rdram, a);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void wrf(uint8_t *rdram, uint32_t a, float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    wr(rdram, a, b);
}
inline uint32_t bitsOf(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// ---- calling the chained handler ----
struct Args
{
    __m128i a[8];
    __m128i ra;
    float f[4];
};
inline void save(const R5900Context *ctx, Args &r)
{
    for (int i = 0; i < 8; ++i)
        r.a[i] = ctx->r[4 + i]; // a0..a3, t0..t3 (MoveCheck takes seven arguments)
    r.ra = ctx->r[31];
    for (int i = 0; i < 4; ++i)
        r.f[i] = ctx->f[12 + i];
}
inline void load(R5900Context *ctx, const Args &r)
{
    for (int i = 0; i < 8; ++i)
        ctx->r[4 + i] = r.a[i];
    ctx->r[31] = r.ra;
    for (int i = 0; i < 4; ++i)
        ctx->f[12 + i] = r.f[i];
}
inline void setReg(R5900Context *ctx, int reg, uint32_t v)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v)));
}

// The runtime yields out of long guest loops by RETURNING with a mid-function resume pc (G57 back-edge
// preemption) and re-enters the body later from its resume table. A wrapper that does anything after
// the call -- restore a field, run the body again, retry with other inputs -- must therefore run the
// body to completion in one call: the G57 scoped suppression, used by every wrapper below that has
// post-work.
struct NoPreempt
{
    NoPreempt() { g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed); }
    ~NoPreempt() { g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed); }
    NoPreempt(const NoPreempt &) = delete;
    NoPreempt &operator=(const NoPreempt &) = delete;
};

// One guest call of `fn` as the call site made it. A generated body leaves pc at its entry or sets
// it to ra on `jr ra`; anything else means the EE unwound inside the call (G787's rule) and the
// wrapper must return at once.
inline bool call(Fn fn, uint32_t entry, uint32_t ra, uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    NoPreempt guard;
    ctx->pc = entry;
    fn(rdram, ctx, rt);
    if (ctx->pc != ra && ctx->pc != entry)
        return false;
    ctx->pc = ra;
    return true;
}

// Runs the chained handler `times` times with the original arguments (0 = return at once).
inline void repeat(Site s, uint32_t entry, uint32_t times, uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (times == 0u)
    {
        ctx->pc = ra;
        return;
    }
    if (times == 1u)
    {
        prev[s](rdram, ctx, rt);
        return;
    }
    Args args;
    save(ctx, args);
    for (uint32_t i = 0; i < times; ++i)
    {
        if (i != 0u)
            load(ctx, args);
        if (!call(prev[s], entry, ra, rdram, ctx, rt))
            return;
    }
}

// Runs a per-tick system once per 60 Hz tick due this frame, each run in a one-tick context. Nested
// (already inside a tick) or with the mode idle it is a plain call.
inline void runTicks(Site s, uint32_t entry, uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    if (st.inTick || !st.on)
    {
        prev[s](rdram, ctx, rt);
        return;
    }
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t n = std::min(st.frame.k60, kMaxTicks);
    if (n == 1u && st.frame.ticks == 1.0f)
    {
        prev[s](rdram, ctx, rt); // a one-tick frame already is the tick context
        return;
    }
    if (n == 0u)
    {
        ++hits[HTickSkip];
        ctx->pc = ra;
        return;
    }
    Args args;
    save(ctx, args);
    const float oneScale = 1.0f / static_cast<float>(st.requested);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i != 0u)
            load(ctx, args);
        st.tick = Ctx{1.0f, 1u, st.tickDue[i], 1u, oneScale, 1.0f};
        st.inTick = true;
        wr(rdram, kMgFrameRate, 1u);
        ++hits[HTickRun];
        const bool ok = call(prev[s], entry, ra, rdram, ctx, rt);
        st.inTick = false;
        wr(rdram, kMgFrameRate, st.frame.intRate);
        if (!ok)
            return;
    }
}

// ---- the frame clock ----
inline Ctx passThroughCtx()
{
    Ctx c;
    c.ticks = static_cast<float>(st.requested);
    c.intRate = st.requested;
    return c;
}

// The game wrote mgFrameRate. Re-asserting the rate it already asks for keeps the clock running.
inline void setRequested(uint32_t v)
{
    if (v < 1u || v > 4u || v == st.requested)
        return;
    st.requested = v;
    st.acc30 = st.acc60 = 0.0;
    st.tickAcc = 0u;
    st.bankUs = st.owedUs = 0.0;
    st.discontinuity = true;
    st.on = false;
    st.frame = passThroughCtx(); // the rest of this frame runs at the rate the game just set
}

// After a game function that writes mgFrameRate (SetMenuFrameRate, TitleModeInit).
inline void afterRateWriter(uint8_t *rdram)
{
    const uint32_t v = rd(rdram, kMgFrameRate);
    setRequested(v);
    if (st.on)
    {
        wr(rdram, kMgFrameRate, st.frame.intRate); // same rate re-asserted: this frame keeps its step
        st.lastWritten = st.frame.intRate;
    }
    else
        st.lastWritten = v;
}

// A loop that is not gameplay (pause, loading, movie, capture, emergency message) runs at the game's
// own pacing; the next MainLoop frame restarts the clock.
inline void useDesignedRate(uint8_t *rdram)
{
    if (st.on)
    {
        wr(rdram, kMgFrameRate, st.requested);
        st.lastWritten = st.requested;
    }
    st.on = false;
    st.frame = passThroughCtx();
    st.discontinuity = true;
}

inline void noteGameRate(uint8_t *rdram)
{
    const uint32_t v = rd(rdram, kMgFrameRate);
    if (v != st.lastWritten)
    {
        setRequested(v);
        st.lastWritten = v;
    }
}

inline void traceFrame(double dtUs)
{
    if (!st.trace)
        return;
    ++st.winFrames;
    st.winTicks += st.frame.ticks;
    st.winDue += st.frame.due30;
    st.winK60 += st.frame.k60;
    st.winTwo += st.frame.ticks > 1.5f ? 1u : 0u;
    st.winUs += dtUs;
    st.winOwed = std::max(st.winOwed, st.owedUs);
    if (st.winFrames < 120u)
        return;
    const double sec = st.winUs / 1e6;
    std::fprintf(stderr,
                 "[G793:dfr] frame=%llu R=%u on=%u fps=%.1f ticks=%.1f (gameSpeed=%.0f%%) "
                 "multiTickFrames=%u due30=%u k60=%u maxOwedMs=%.1f pots=%zu\n",
                 static_cast<unsigned long long>(st.frameNo), st.requested, st.on ? 1u : 0u,
                 sec > 0.0 ? st.winFrames / sec : 0.0, st.winTicks,
                 sec > 0.0 ? 100.0 * (st.winTicks * st.periodUs / 1e6) / sec : 0.0, st.winTwo,
                 st.winDue, st.winK60, st.winOwed / 1000.0, st.pots.size());
    st.winFrames = st.winTwo = st.winDue = st.winK60 = 0u;
    st.winTicks = st.winUs = st.winOwed = 0.0;
    char line[640];
    int len = std::snprintf(line, sizeof(line), "[G793:hits]");
    for (uint32_t i = 0; i < HCount && len > 0 && len < static_cast<int>(sizeof(line)); ++i)
        len += std::snprintf(line + len, sizeof(line) - len, " %s=%llu", kHitNames[i],
                             static_cast<unsigned long long>(hits[i]));
    std::fprintf(stderr, "%s\n", line);
}

inline void presentEventInput(uint8_t *rdram, uint64_t endedFrame);

// The frame clock, ticked once per MainLoop frame from its pad read (UpDate), i.e. after the frame's
// game logic and before its draw / vblank wait: choose the NEXT frame's step from the real length of
// the last full frame. Ticking here (not in mgEndFrame) lets the next frame's input be prepared before
// MainLoop's own CPadControl::Update reads it.
inline void clockTick(uint8_t *rdram)
{
    const auto now = std::chrono::steady_clock::now();
    const uint64_t endedFrame = st.frameNo;
    double dtUs = st.periodUs;
    if (st.haveLast)
        dtUs = std::chrono::duration<double, std::micro>(now - st.lastEnd).count();
    st.lastEnd = now;
    st.haveLast = true;
    ++st.frameNo;

    noteGameRate(rdram); // a write after mgEndFrame's own read (rare) still counts
    // Recorded-input playback (attract demo, CaptureMode 2/3) is frame-exact by construction.
    const bool live = st.requested == 2u && rd(rdram, kPadCapture) != 2u;
    if (!live)
    {
        st.on = false;
        st.acc30 = st.acc60 = 0.0;
        st.tickAcc = 0u;
        st.bankUs = st.owedUs = 0.0;
        st.frame = passThroughCtx();
        st.sideFrames = 0u;
        st.presentValid = false;
        traceFrame(dtUs);
        return;
    }

    const double P = st.periodUs;
    const double capT = static_cast<double>(st.cap);
    double T = 1.0;
    if (st.mode != Mode60)
    {
        if (st.discontinuity || st.sideFrames != 0u || dtUs > 250000.0)
        {
            // loading / pause / movie / a stall: nothing is owed for it
            st.bankUs = st.owedUs = 0.0;
            dtUs = st.hostFps != 0u ? 1e6 / st.hostFps : P;
        }
        st.bankUs += dtUs;
        if (st.mode == ModeDynamic)
        {
            // Whole ticks. Bias 0.25: a frame takes a second tick only once 1.75 periods are banked,
            // so ordinary jitter around one vblank never produces a 2-tick frame.
            T = std::max(1.0, std::min(std::floor(st.bankUs / P + 0.25), capT));
            st.bankUs -= T * P;
            if (st.owedUs >= P && T < capT) // repay an earlier hitch, a whole tick at a time
            {
                const double extra = std::min(capT - T, std::floor(st.owedUs / P));
                T += extra;
                st.owedUs -= extra * P;
            }
        }
        else
        {
            T = std::min(st.bankUs / P, capT); // fractional ticks
            st.bankUs -= T * P;
            if (st.owedUs > 0.0 && T < capT) // repay, at most doubling this frame's step
            {
                const double extra = std::min({capT - T, T, st.owedUs / P});
                T += extra;
                st.owedUs -= extra * P;
            }
        }
        // What the cap could not pay is owed -- but only for a SPIKE (a frame longer than cap + 1
        // ticks: a loading hitch inside a cutscene). Running steadily below 30 fps is the original
        // game's slowdown, and repaying it later would fast-forward the next smooth stretch.
        const double carry = st.mode == ModeDynamic ? P : 0.0;
        if (st.bankUs > carry)
        {
            if (dtUs > (capT + 1.0) * P)
                st.owedUs = std::min(st.owedUs + (st.bankUs - carry), st.catchupUs);
            st.bankUs = carry;
        }
        st.bankUs = std::max(-P, st.bankUs);
    }
    st.discontinuity = false;
    st.sideFrames = 0u;
    st.on = true;

    Ctx c;
    c.ticks = static_cast<float>(T);
    c.intRate = std::max(1u, static_cast<uint32_t>(std::floor(T + 1e-4)));
    c.moveScale = static_cast<float>(T / c.intRate);
    c.scale = static_cast<float>(T / st.requested);
    st.acc30 += T;
    c.due30 = static_cast<uint32_t>(std::floor(st.acc30 / st.requested + 1e-6));
    st.acc30 -= static_cast<double>(c.due30) * st.requested;
    st.acc60 += T;
    c.k60 = static_cast<uint32_t>(std::floor(st.acc60 + 1e-6));
    st.acc60 -= c.k60;
    c.k60 = std::min(c.k60, kMaxTicks);
    for (uint32_t i = 0; i < c.k60; ++i)
    {
        st.tickDue[i] = (++st.tickAcc >= st.requested) ? 1u : 0u;
        if (st.tickDue[i] != 0u)
            st.tickAcc = 0u;
    }
    st.frame = c;
    wr(rdram, kMgFrameRate, c.intRate);
    st.lastWritten = c.intRate;
    presentEventInput(rdram, endedFrame);
    traceFrame(dtUs);
}

// Host pacing for rates above 60: sleep (guest lock released) until one frame period after the last.
inline void paceHost(PS2Runtime *rt)
{
    if (st.hostFps == 0u || !st.havePaced)
        return;
    using clock = std::chrono::steady_clock;
    const auto deadline = st.lastPaced + std::chrono::microseconds(static_cast<int64_t>(1e6 / st.hostFps));
    if (clock::now() >= deadline)
        return;
    PS2Runtime::GuestExecutionReleaseScope release(rt);
    const auto coarse = deadline - std::chrono::microseconds(1500);
    if (clock::now() < coarse)
        std::this_thread::sleep_until(coarse);
    while (clock::now() < deadline)
        std::this_thread::yield();
}

// INPUT DURING EVENTS (pnach known issue 2). The event script runs once per 30 fps frame, but the
// pad was sampled every frame, so a press or release whose edge fell on a skipped frame never
// reached the script ("the game thinks the button is still held"). Giving the script alone its own
// edges desynchronised the dialogue: the message window (per frame) and the script both react to
// the button and must see it in the SAME frame, as at 30 fps. So while an event runs, the edges
// are presented to everyone on 30 fps frames only: a frame with no 30 fps step sees the pad as
// held (prev = cur, no edges); the next one sees every change since the last presented frame.
// That is the original game's input cadence (the pnach's Spheda hack, generalised). Runs at the
// end of a MainLoop frame, after UpDate and CPadControl::Update, for the frame about to start.
inline void presentEventInput(uint8_t *rdram, uint64_t endedFrame)
{
    const bool active = fix(FEdges) && st.on && st.eventValid && st.eventFrame == endedFrame;
    if (!active)
    {
        st.presentValid = false;
        return;
    }
    const uint32_t cur0 = rd(rdram, kGamePad + 0x04u), cur1 = rd(rdram, kGamePad + 0x50u);
    const uint32_t prev0 = rd(rdram, kGamePad + 0x9Cu), prev1 = rd(rdram, kGamePad + 0xE8u);
    if (!st.presentValid)
    {
        st.base0 = prev0; // an event is starting: nothing earlier is presented again
        st.base1 = prev1;
        st.presentValid = true;
    }
    uint32_t np0 = cur0, np1 = cur1;
    if (st.frame.due30 != 0u)
    {
        np0 = st.base0;
        np1 = st.base1;
        st.base0 = cur0;
        st.base1 = cur1;
    }
    if (np0 == prev0 && np1 == prev1)
        return;
    wr(rdram, kGamePad + 0x9Cu, np0); // MainLoop's CPadControl::Update, next, rebuilds the Btn table
    wr(rdram, kGamePad + 0xE8u, np1);
    ++hits[HEdgeSwap];
}

// ---- wrappers ----

// mgEndFrame: the frame clock. While the mode is on the vblank wait is one vblank (60 fps cap), or
// none at all above 60 where the host paces; the step for the next frame is this one's real length.
void endFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    noteGameRate(rdram);
    if (ra != kRaMainLoop)
    {
        ++st.sideFrames;
        ++hits[HSide];
        useDesignedRate(rdram);
        prev[SEndFrame](rdram, ctx, rt);
        return;
    }
    const bool paced = st.on && st.hostFps != 0u;
    if (st.on)
    {
        wr(rdram, kMgFrameRate, 1u);
        st.lastWritten = 1u;
    }
    prev[SEndFrame](rdram, ctx, rt);
    if (paced)
        paceHost(rt);
    st.lastPaced = std::chrono::steady_clock::now();
    st.havePaced = true;
    if (st.on)
    {
        wr(rdram, kMgFrameRate, st.frame.intRate); // the step chosen at the pad read
        st.lastWritten = st.frame.intRate;
    }
}

// WaitVSync: above 60 the host paces MainLoop frames, so mgEndFrame's vblank wait is skipped.
void waitVSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    if (st.on && st.hostFps != 0u && getRegU32(ctx, 31) == kRaEndFrameWait)
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    prev[SWaitVSync](rdram, ctx, rt);
}

// mgGetNowFrameRate: (float)mgFrameRate, i.e. the step as a float -- fractional at high rates.
// Readers: MainLoop's sndStep, the edit-mode chara/camera controls, NormalDrive (unused result).
void nowRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    if (!st.on)
    {
        prev[SNowRate](rdram, ctx, rt);
        return;
    }
    ctx->f[0] = cur().ticks;
    ctx->pc = getRegU32(ctx, 31);
}

void setMenuRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    prev[SSetMenuRate](rdram, ctx, rt);
    afterRateWriter(rdram);
}

void titleInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    prev[STitleInit](rdram, ctx, rt);
    afterRateWriter(rdram);
}

// FMV (pnach "Fix Subtitles in FMV", keyed there on the CD-stream flag the HLE never sets): the
// movie loop paces itself with mgFrameRate, so it runs at the game's own rate.
void loadMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    useDesignedRate(rdram);
    prev[SLoadMovie](rdram, ctx, rt);
    st.discontinuity = true;
}

// Character motion: frame += step * 1.0 and blend += speed per frame, tuned for 30 fps. Scale the
// two per-frame fields for the call; keep any value the call itself stored (a new motion).
void normalDrive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const float s = cur().scale;
    if (s == 1.0f)
    {
        prev[SNormalDrive](rdram, ctx, rt);
        return;
    }
    const uint32_t self = getRegU32(ctx, 4);
    const float step = rdf(rdram, self + 0x390u);
    const float blend = rdf(rdram, self + 0x50Cu);
    const float sStep = step * s;
    const float sBlend = blend * s;
    wrf(rdram, self + 0x390u, sStep);
    wrf(rdram, self + 0x50Cu, sBlend);
    ++hits[HAnim];
    {
        NoPreempt guard;
        prev[SNormalDrive](rdram, ctx, rt);
    }
    if (rd(rdram, self + 0x390u) == bitsOf(sStep))
        wrf(rdram, self + 0x390u, step);
    if (rd(rdram, self + 0x50Cu) == bitsOf(sBlend))
        wrf(rdram, self + 0x50Cu, blend);
}

// sceVu0ScaleVectorXYZ(f12 = speed, out, dir): StepParam's blow move adds dir * speed per frame.
void scaleXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const float s = cur().scale;
    if (getRegU32(ctx, 31) == kRaBlowMove && s != 1.0f)
    {
        ctx->f[12] *= s;
        ++hits[HBlow];
    }
    prev[SScaleXYZ](rdram, ctx, rt);
}

// sceVu0ScaleVector(f12 = speed, out, dir): projectile travel per frame.
void scaleVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const float s = cur().scale;
    if ((ra == kRaRocketMove || ra == kRaLaserMove) && s != 1.0f)
    {
        ctx->f[12] *= s;
        ++hits[HWeapon];
    }
    prev[SScaleVec](rdram, ctx, rt);
}

// mgVectorInterpolate(f12 = max step, out, from, to, 0): projectile homing turn per frame.
void vecInterp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const float s = cur().scale;
    if ((ra == kRaRocketTurn || ra == kRaLaserTurn) && s != 1.0f)
    {
        ctx->f[12] *= s;
        ++hits[HHoming];
    }
    prev[SVecInterp](rdram, ctx, rt);
}

// Thrown pots: Throw stores the 30 fps launch (vel 10*sin, 2.5, 10*cos; gravity -0.5) and FlyStep
// integrates once per frame and expires at 150 frames. A pot thrown while the mode is on gets the
// pnach's 60 Hz constants (vel halved, vy 1.3125, gravity -0.125, 300 ticks) and is stepped once per
// 60 Hz tick, which reproduces the 30 fps trajectory exactly at every 30 fps frame boundary.
void potThrow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t self = getRegU32(ctx, 4);
    {
        NoPreempt guard;
        prev[SPotThrow](rdram, ctx, rt);
    }
    if (!st.on || rd(rdram, self) != 2u || rd(rdram, self + 4u) == 0u)
        return;
    if (rd(rdram, self + 0x24u) == bitsOf(2.5f)) // the launch branch ran (a character was found)
    {
        wrf(rdram, self + 0x20u, rdf(rdram, self + 0x20u) * 0.5f);
        wrf(rdram, self + 0x24u, 1.3125f);
        wrf(rdram, self + 0x28u, rdf(rdram, self + 0x28u) * 0.5f);
    }
    wrf(rdram, self + 0x34u, -0.125f);
    if (st.pots.size() > 256u)
        st.pots.clear();
    st.pots[self] = 0u;
    ++hits[HPotThrow];
}

void potFly(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t self = getRegU32(ctx, 4);
    if (st.pots.find(self) == st.pots.end())
    {
        prev[SPotFly](rdram, ctx, rt);
        return;
    }
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t ticks = cur().k60;
    Args args;
    save(ctx, args);
    uint32_t v0 = 0u;
    for (uint32_t i = 0; i < ticks; ++i)
    {
        if (i != 0u)
        {
            if (rd(rdram, self) != 2u || rd(rdram, self + 4u) == 0u)
                break;
            load(ctx, args);
        }
        // FlyStep does `life += 1; if (life >= 150) expire`. Feeding it floor(n/2) - 1 before tick n
        // expires the pot at tick 300 and keeps `life` on the 30 fps count for everything else.
        const uint32_t n = ++st.pots[self];
        ++hits[HPotTick];
        wr(rdram, self + 0x70u, static_cast<uint32_t>(static_cast<int32_t>(n / 2u) - 1));
        if (!call(prev[SPotFly], kFnPotFly, ra, rdram, ctx, rt))
            return;
        v0 = getRegU32(ctx, 2);
        if (v0 != 0u)
        {
            st.pots.erase(self);
            break;
        }
    }
    setReturnU32(ctx, v0);
    ctx->pc = ra;
}

// EventLoop's script resume (pnach "New Object Loop Limiter"): once per 30 fps frame. Its input is
// handled for the whole frame by presentEventInput below.
void eventResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    st.eventValid = true;
    st.eventFrame = st.frameNo;
    const uint32_t times = fix(FCutscene) ? cur().due30 : 1u;
    hits[HEventRun] += times;
    hits[HEventSkip] += times == 0u ? 1u : 0u;
    repeat(SResume, kFnResume, times, rdram, ctx, rt);
}

void resume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (ra == kRaEventLoop && (st.fixes & (FCutscene | FEdges)) != 0u)
    {
        eventResume(rdram, ctx, rt);
        return;
    }
    if (ra == kRaMonsterRun && fix(FStun))
    {
        // a0 = monster + 0x1040 (its CRunScript). The stun script waits in 30 fps frames.
        const uint32_t monster = getRegU32(ctx, 4) - 0x1040u;
        if (rd16(rdram, monster + 0x115Au) == kMonsterStunScript)
        {
            const uint32_t due = cur().due30;
            hits[HStun] += due != 1u ? 1u : 0u;
            repeat(SResume, kFnResume, due, rdram, ctx, rt);
            return;
        }
    }
    prev[SResume](rdram, ctx, rt);
}

// EdEventStep's 32 scene-object sequences (pnach 0x26237C).
void objSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t due = cur().due30;
    hits[HSeqSkip] += due == 0u ? 1u : 0u;
    repeat(SObjSeq, kFnObjSeq, due, rdram, ctx, rt);
}

// The scene camera sequence (pnach 0x259520): with no 30 fps frame due the camera is still evaluated
// but the five command tracks do not advance. Nulling the track heads makes the generated loop skip
// them; it also zeroes each track's companion word, so all ten words are restored afterwards.
void cmrSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t due = cur().due30;
    if (due != 0u)
    {
        repeat(SCmrSeq, kFnCmrSeq, due, rdram, ctx, rt);
        return;
    }
    ++hits[HCmrSkip];
    const uint32_t self = getRegU32(ctx, 4);
    uint32_t tracks[10];
    for (uint32_t i = 0; i < 10u; ++i)
        tracks[i] = rd(rdram, self + 8u + 4u * i);
    for (uint32_t t = 0; t < 5u; ++t)
        wr(rdram, self + 8u + 8u * t, 0u);
    {
        NoPreempt guard;
        prev[SCmrSeq](rdram, ctx, rt);
    }
    for (uint32_t i = 0; i < 10u; ++i)
        wr(rdram, self + 8u + 4u * i, tracks[i]);
}

// CScreenEffect::Step (pnach 0x260630) and CPowGage::Step (pnach 0x2EA06C, Spheda cursor).
void screenEff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    repeat(SScreenEff, kFnScreenEff, cur().due30, rdram, ctx, rt);
}
void powGage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t due = cur().due30;
    hits[HGauge] += due != 1u ? 1u : 0u;
    repeat(SPowGage, kFnPowGage, due, rdram, ctx, rt);
}

// CPiyori::Step, the dizzy birds (pnach 0x1C9B5C/0x1C9B7C/0x1C9BF4/0x1C9C44): the duration and the
// chirp interval count 30 fps frames; the spin (Pi/30 and 2Pi/30 per frame) follows real time.
inline float piyoriWrap(float a, float d)
{
    const float kPi = 3.14159274f, k2Pi = 6.28318548f;
    float f = a + d;
    if (!(f <= kPi))
        f -= k2Pi;
    return f;
}
void piyori(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t self = getRegU32(ctx, 4);
    const Ctx &c = cur();
    if ((c.due30 == 1u && c.scale == 1.0f) || rd(rdram, self) == 0u)
    {
        prev[SPiyori](rdram, ctx, rt);
        return;
    }
    const float s = c.scale;
    const float spin = rdf(rdram, self + 0x10u);
    float orbit[3];
    for (uint32_t i = 0; i < 3u; ++i)
        orbit[i] = rdf(rdram, self + 4u + 4u * i);
    ++hits[HPiyori];
    uint32_t calls = c.due30;
    if (calls == 0u)
    {
        // No 30 fps frame due: the call's two decrements net to zero.
        wr16(rdram, self + 0x1Cu, static_cast<uint16_t>(rd16(rdram, self + 0x1Cu) + 1u));
        wr16(rdram, self + 0x1Eu, static_cast<uint16_t>(rd16(rdram, self + 0x1Eu) + 1u));
        calls = 1u;
    }
    {
        NoPreempt guard;
        repeat(SPiyori, kFnPiyori, calls, rdram, ctx, rt);
    }
    if (rd(rdram, self) != 0u)
    {
        wrf(rdram, self + 0x10u, piyoriWrap(spin, 0.104719758f * s));
        for (uint32_t i = 0; i < 3u; ++i)
            wrf(rdram, self + 4u + 4u * i, piyoriWrap(orbit[i], 0.209439516f * s));
    }
}

inline bool movingXZ(uint8_t *rdram, uint32_t vec) { return (rd(rdram, vec) | rd(rdram, vec + 8u)) != 0u; }
inline bool grounded(uint8_t *rdram, uint32_t chara) { return rd(rdram, chara + kCharaMoveInfo + 8u) != 0u; }
inline bool shortStep() { return st.on && cur().ticks < 1.99f; } // a frame's move shorter than at 30 fps

// RockOn runs at the end of every MoveIF, right after it stored this frame's move vector at +0x80.
//  * high rates: the MoveIF scaled the move by mgFrameRate (an integer); scale it by what is missing.
//  * `lift` / `pnach` stair modes: the pnach's cave (it tested the vector before the copy to +0x80).
void rockOn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    st.player = self; // every MoveIF ends here, and every MoveIF is the player's
    const float ms = cur().moveScale;
    if (fix(FMove) && st.on && ms != 1.0f)
    {
        st.vyRaw = rdf(rdram, self + 0x84u);
        st.vyScaled = st.vyRaw * ms;
        st.vyValid = true;
        wrf(rdram, self + 0x80u, rdf(rdram, self + 0x80u) * ms);
        wrf(rdram, self + 0x84u, st.vyScaled);
        wrf(rdram, self + 0x88u, rdf(rdram, self + 0x88u) * ms);
        ++hits[HMove];
    }
    if (fix(FStairs) && ra == kRaHumanMove && shortStep() &&
        (st.stairs == StairsLift || st.stairs == StairsPnach) && movingXZ(rdram, self + 0x80u))
    {
        const bool ok = st.stairs == StairsPnach
                            ? (rd(rdram, kStairFlagA) == 0u && rd(rdram, kStairFlagB) != 0u)
                            : grounded(rdram, self);
        if (ok)
        {
            wrf(rdram, self + 0x14u, rdf(rdram, self + 0x14u) + kStairLift);
            ++hits[HStairs];
        }
    }
    prev[SRockOn](rdram, ctx, rt);
}

inline void stairTrace(uint8_t *rdram, const char *tag, uint32_t pos, uint32_t move, uint32_t out,
                       uint32_t info, bool groundedBefore)
{
    if (!st.stairTrace || st.stairLines >= 20000u)
        return;
    ++st.stairLines;
    std::fprintf(stderr,
                 "[G793:stair] f=%llu %s T=%.2f pos=(%.2f,%.2f,%.2f) move=(%.3f,%.3f,%.3f) "
                 "out=(%.2f,%.2f,%.2f) gnd=%u->%u width=%u flagA=0x%x flagB=0x%x chara=0x%x\n",
                 static_cast<unsigned long long>(st.frameNo), tag, cur().ticks, rdf(rdram, pos),
                 rdf(rdram, pos + 4u), rdf(rdram, pos + 8u), rdf(rdram, move), rdf(rdram, move + 4u),
                 rdf(rdram, move + 8u), rdf(rdram, out), rdf(rdram, out + 4u), rdf(rdram, out + 8u),
                 groundedBefore ? 1u : 0u, rd(rdram, info + 8u) != 0u ? 1u : 0u,
                 rd(rdram, info + 0xD0u), rd(rdram, kStairFlagA), rd(rdram, kStairFlagB),
                 info - kCharaMoveInfo);
}

inline float distXZ(uint8_t *rdram, uint32_t a, uint32_t b)
{
    const float dx = rdf(rdram, a) - rdf(rdram, b), dz = rdf(rdram, a + 8u) - rdf(rdram, b + 8u);
    return std::sqrt(dx * dx + dz * dz);
}

// MoveCheck(pos, move, out, info, polys, n, flags) for the player (CActionChara::RunScript).
// `stride`: MoveCheck only snaps the player onto a higher floor when this frame's target point lies
// past the edge. At 30 fps one step reaches past the lip at the top of a ramp; a shorter step lands
// on the lip, finds no floor, and the width test pushes the player back -- every frame. When a short
// step is blocked, the 30 fps-length step is tried as well and kept only if it lands HIGHER.
void moveCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool stride = fix(FStairs) && st.stairs == StairsStride;
    if (ra != kRaCharaMove || (!stride && !st.stairTrace))
    {
        prev[SMoveCheck](rdram, ctx, rt);
        return;
    }
    const uint32_t pos = getRegU32(ctx, 4), move = getRegU32(ctx, 5), out = getRegU32(ctx, 6);
    const uint32_t info = getRegU32(ctx, 7);
    const bool moving = movingXZ(rdram, move);
    const bool gndBefore = rd(rdram, info + 8u) != 0u;
    const float stepScale = static_cast<float>(st.requested) / std::max(0.05f, cur().ticks);
    const bool tryStride = stride && shortStep() && moving && gndBefore && stepScale > 1.01f;
    // Inputs MoveCheck reads and writes: the move (halved on a blocked probe), the out vector and
    // the whole MoveCheckInfo (0x110 bytes, MoveCheckInfo::Initialize).
    constexpr uint32_t kInfoBytes = 0x110u;
    auto grab = [&](uint32_t a, uint8_t *dst, uint32_t n) {
        if (const uint8_t *m = getConstMemPtr(rdram, a))
            std::memcpy(dst, m, n);
    };
    auto put = [&](uint32_t a, const uint8_t *src, uint32_t n) {
        if (uint8_t *m = getMemPtr(rdram, a))
            std::memcpy(m, src, n);
    };
    uint8_t moveIn[16], infoIn[kInfoBytes];
    Args args;
    if (tryStride)
    {
        grab(move, moveIn, 16);
        grab(info, infoIn, kInfoBytes);
        save(ctx, args);
    }
    const float moveLen = std::sqrt(rdf(rdram, move) * rdf(rdram, move) + rdf(rdram, move + 8u) * rdf(rdram, move + 8u));
    if (!call(prev[SMoveCheck], kFnMoveCheck, ra, rdram, ctx, rt))
        return;
    const float y0 = rdf(rdram, pos + 4u);
    const float went = distXZ(rdram, out, pos);
    const bool landed = rd(rdram, info + 8u) != 0u;
    const bool blocked = went < 0.5f * moveLen || !landed;
    if (!tryStride || !blocked)
    {
        if (moving)
            stairTrace(rdram, "plain", pos, move, out, info, gndBefore);
        return;
    }
    uint8_t outN[16], moveN[16], infoN[kInfoBytes];
    grab(out, outN, 16);
    grab(move, moveN, 16);
    grab(info, infoN, kInfoBytes);
    put(move, moveIn, 16);
    put(info, infoIn, kInfoBytes);
    wrf(rdram, move, rdf(rdram, move) * stepScale);
    wrf(rdram, move + 8u, rdf(rdram, move + 8u) * stepScale);
    load(ctx, args);
    if (!call(prev[SMoveCheck], kFnMoveCheck, ra, rdram, ctx, rt))
        return;
    const bool climbed = rd(rdram, info + 8u) != 0u && rdf(rdram, out + 4u) > y0 + 0.1f &&
                         distXZ(rdram, out, pos) > went + 0.25f;
    if (climbed)
    {
        ++hits[HStairs];
        stairTrace(rdram, "stride-up", pos, move, out, info, gndBefore);
        return;
    }
    put(out, outN, 16); // not a step up (a wall, a ledge down): the shipped result stands
    put(move, moveN, 16);
    put(info, infoN, kInfoBytes);
    ++hits[HStairFallback];
    stairTrace(rdram, "blocked", pos, move, out, info, gndBefore);
}

// The player's RunScript at fractional steps: RockOn scaled this frame's vertical displacement; the
// script then applied one frame of gravity (-0.5 / -3.0, clamped) to it. Re-base that change on the
// unscaled velocity and scale it by the frame's share of a tick.
void charaScript(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t self = getRegU32(ctx, 4);
    st.vyValid = false;
    {
        NoPreempt guard;
        prev[SCharaScript](rdram, ctx, rt);
    }
    if (!st.vyValid || self != st.player)
        return;
    st.vyValid = false;
    const float after = rdf(rdram, self + 0x84u);
    if (after == 0.0f)
        return; // grounded: the script zeroes it
    const float ms = cur().moveScale;
    float v = st.vyRaw + (after - st.vyScaled) * ms;
    if (after <= -3.49f && v < after)
        v = after; // the script's terminal velocity
    wrf(rdram, self + 0x84u, v);
    ++hits[HGravity];
}

// Texture animation (pnach known issue 1): frames advance only when a 30 fps frame is due, through
// the game's own stop_anime switch, so the animated textures are still drawn every frame.
void texAnime(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    if (cur().due30 == 0u && rd(rdram, kStopAnime) == 0u)
    {
        wr(rdram, kStopAnime, 1u);
        ++hits[HTexStop];
        {
            NoPreempt guard;
            prev[STexAnime](rdram, ctx, rt);
        }
        if (rd(rdram, kStopAnime) == 1u)
            wr(rdram, kStopAnime, 0u);
        return;
    }
    prev[STexAnime](rdram, ctx, rt);
}

// CGamePad::UpDate. MainLoop's per-frame call ticks the frame clock right after the pad is read.
// Auto-repeat: the delay and interval are counted in frames (SetAutoRepeat), so a held button
// repeated twice as fast at 60 fps -- both are scaled by R / T for the call.
void padUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const float T = st.frame.ticks;
    NoPreempt guard; // the clock below reads the pad state this call produces
    if (!fix(FRepeat) || !st.on || T == static_cast<float>(st.requested))
    {
        prev[SPadUpdate](rdram, ctx, rt);
    }
    else
    {
        const uint32_t pad = getRegU32(ctx, 4);
        struct Saved
        {
            uint32_t addr, value;
        } saved[128];
        uint32_t n = 0u;
        const float factor = static_cast<float>(st.requested) / T;
        for (uint32_t p = 0; p < 2u; ++p)
        {
            const uint32_t base = pad + 0x188u * p;
            const uint32_t mask = rd(rdram, base + 0x144u);
            for (uint32_t b = 0; b < 32u && mask != 0u; ++b)
            {
                if ((mask & (1u << b)) == 0u)
                    continue;
                for (const uint32_t off : {0x1CCu, 0x24Cu})
                {
                    const uint32_t a = base + off + 4u * b;
                    const uint32_t v = rd(rdram, a);
                    const int32_t iv = static_cast<int32_t>(v);
                    if (iv <= 0)
                        continue;
                    const int32_t scaled = std::max<int32_t>(
                        1, static_cast<int32_t>(std::lround(static_cast<float>(iv) * factor)));
                    saved[n++] = {a, v};
                    ++hits[HRepeat];
                    wr(rdram, a, static_cast<uint32_t>(scaled));
                }
            }
        }
        prev[SPadUpdate](rdram, ctx, rt);
        for (uint32_t i = 0; i < n; ++i)
            wr(rdram, saved[i].addr, saved[i].value);
    }
    if (ra == kRaMainLoopPad)
        clockTick(rdram);
}

// Per-tick systems. DngStep is the whole world step (monsters, effects, items, damage numbers, ...):
// ticked above 60 only, where the frame itself is shorter than a tick. ThinkHost (monster AI and
// motion) and the effect scripts are ticked at every rate, so a 2-tick frame runs them twice.
void dngStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    if (st.hostFps == 0u)
    {
        prev[SDngStep](rdram, ctx, rt);
        return;
    }
    runTicks(SDngStep, kFnDngStep, rdram, ctx, rt);
}
void thinkHost(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    runTicks(SThinkHost, kFnThinkHost, rdram, ctx, rt);
}
void effectStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    runTicks(SEffectStep, kFnEffectStep, rdram, ctx, rt);
}

// ---- configuration ----
inline std::string lower(const char *s)
{
    std::string out = s ? s : "";
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}
inline bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline void configure()
{
    const std::string mode = lower(std::getenv("DC2_FRAMERATE"));
    char *end = nullptr;
    const long fps = mode.empty() ? 0L : std::strtol(mode.c_str(), &end, 10);
    const bool numeric = !mode.empty() && end != nullptr && end != mode.c_str() &&
                         (*end == '\0' || std::string(end) == "fps");
    if (mode.empty() || mode == "off" || mode == "original" || (numeric && fps <= 30))
        st.mode = ModeOff;
    else if (mode == "dynamic" || mode == "dyn" || mode == "auto" || mode == "variable" || mode == "vrr")
        st.mode = ModeDynamic;
    else if (mode == "fixed60" || (numeric && fps == 60))
        st.mode = Mode60;
    else if (numeric && fps > 60 && fps <= 360)
    {
        st.mode = ModeHigh;
        st.hostFps = static_cast<uint32_t>(fps);
    }
    else
    {
        std::fprintf(stderr,
                     "[G793:dfr] DC2_FRAMERATE=%s not understood (30 | 60 | dynamic | 72 | 120 | 144 | 240); "
                     "mode off\n",
                     mode.c_str());
        st.mode = ModeOff;
    }
    if (const char *c = std::getenv("DC2_FRAMERATE_MAX_STEP"))
    {
        const long v = std::strtol(c, nullptr, 10);
        st.cap = static_cast<uint32_t>(std::max(2L, std::min(v, 4L)));
    }
    if (const char *c = std::getenv("DC2_FRAMERATE_CATCHUP_MS"))
    {
        const long v = std::strtol(c, nullptr, 10);
        st.catchupUs = static_cast<double>(std::max(0L, std::min(v, 1000L))) * 1000.0;
    }
    const std::string stairs = lower(std::getenv("DC2_FRAMERATE_STAIRS"));
    if (stairs == "off" || stairs == "0")
        st.stairs = StairsOff;
    else if (stairs == "pnach")
        st.stairs = StairsPnach;
    else if (stairs == "lift")
        st.stairs = StairsLift;
    else
        st.stairs = StairsStride;
    st.fixes = FAll;
    if (st.stairs == StairsOff)
        st.fixes &= ~static_cast<uint32_t>(FStairs);
    const std::string off = lower(std::getenv("DC2_FRAMERATE_NO"));
    for (const FixName &f : kFixNames)
    {
        const std::string name = f.name;
        size_t pos = off.find(name);
        while (pos != std::string::npos)
        {
            const bool startOk = pos == 0u || !alpha(off[pos - 1u]);
            const size_t e = pos + name.size();
            const bool endOk = e >= off.size() || !alpha(off[e]);
            if (startOk && endOk)
            {
                st.fixes &= ~f.bit;
                break;
            }
            pos = off.find(name, pos + 1u);
        }
    }
    st.trace = dc2_env_flag_enabled("DC2_FRAMERATE_TRACE");
    st.stairTrace = dc2_env_flag_enabled("DC2_FRAMERATE_STAIR_TRACE");
    if (const char *v = std::getenv("DC2_G769_VBLANK_US")) // same period as the guest vblank worker
    {
        const long us = std::strtol(v, nullptr, 10);
        if (us >= 2000L && us <= 100000L)
            st.periodUs = static_cast<double>(us);
    }
}
} // namespace g793

static void g793_register(PS2Runtime &runtime)
{
    using namespace g793;
    configure();
    if (st.mode == ModeOff)
        return;
    uint32_t armed = 0u;
    auto hook = [&](Site s, uint32_t addr, Fn w) {
        if (!runtime.hasFunction(addr))
        {
            std::fprintf(stderr, "[G793:dfr] no function at 0x%08x - hook skipped\n", addr);
            return;
        }
        prev[s] = runtime.lookupFunction(addr);
        runtime.registerFunction(addr, w);
        ++armed;
    };
    hook(SEndFrame, kFnEndFrame, &endFrame);
    if (!prev[SEndFrame])
    {
        std::fprintf(stderr, "[G793:dfr] mgEndFrame not hookable - frame-rate mode disabled\n");
        st.mode = ModeOff;
        return;
    }
    if (st.hostFps != 0u)
        hook(SWaitVSync, kFnWaitVSync, &waitVSync);
    hook(SNowRate, kFnNowRate, &nowRate);
    hook(SSetMenuRate, kFnSetMenuRate, &setMenuRate);
    hook(STitleInit, kFnTitleInit, &titleInit);
    hook(SLoadMovie, kFnLoadMovie, &loadMovie);
    if (fix(FAnim))
        hook(SNormalDrive, kFnNormalDrive, &normalDrive);
    if (fix(FBlow))
        hook(SScaleXYZ, kFnScaleXYZ, &scaleXYZ);
    if (fix(FWeapon))
        hook(SScaleVec, kFnScaleVec, &scaleVec);
    if (fix(FHoming))
        hook(SVecInterp, kFnVecInterp, &vecInterp);
    if (fix(FPot))
    {
        hook(SPotThrow, kFnPotThrow, &potThrow);
        hook(SPotFly, kFnPotFly, &potFly);
    }
    if ((st.fixes & (FCutscene | FEdges | FStun)) != 0u)
        hook(SResume, kFnResume, &resume);
    if (fix(FCutscene))
    {
        hook(SObjSeq, kFnObjSeq, &objSeq);
        hook(SCmrSeq, kFnCmrSeq, &cmrSeq);
        hook(SScreenEff, kFnScreenEff, &screenEff);
    }
    if (fix(FPiyori))
        hook(SPiyori, kFnPiyori, &piyori);
    if (fix(FGauge))
        hook(SPowGage, kFnPowGage, &powGage);
    if ((st.fixes & (FStairs | FMove)) != 0u)
        hook(SRockOn, kFnRockOn, &rockOn);
    if ((fix(FStairs) && st.stairs == StairsStride) || st.stairTrace)
        hook(SMoveCheck, kFnMoveCheck, &moveCheck);
    if (fix(FMove) && st.hostFps != 0u)
        hook(SCharaScript, kFnCharaScript, &charaScript);
    if (fix(FTexAnime))
        hook(STexAnime, kFnTexAnime, &texAnime);
    hook(SPadUpdate, kFnPadUpdate, &padUpdate); // the frame clock
    if (fix(FTicks))
    {
        if (st.hostFps != 0u)
            hook(SDngStep, kFnDngStep, &dngStep);
        hook(SThinkHost, kFnThinkHost, &thinkHost);
        hook(SEffectStep, kFnEffectStep, &effectStep);
    }
    static const char *const kStairNames[] = {"off", "stride", "lift", "pnach"};
    std::fprintf(stderr,
                 "[G793:dfr] mode=%s fps=%u maxStep=%u catchupMs=%.0f stairs=%s fixes=0x%04x hooks=%u "
                 "periodUs=%.0f\n",
                 st.mode == Mode60 ? "60" : st.mode == ModeDynamic ? "dynamic" : "high",
                 st.hostFps != 0u ? st.hostFps : 60u, st.cap, st.catchupUs / 1000.0,
                 kStairNames[fix(FStairs) ? st.stairs : 0u], st.fixes, armed, st.periodUs);
}
