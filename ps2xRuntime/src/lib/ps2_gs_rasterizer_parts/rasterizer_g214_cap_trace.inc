// PHASE G214 (default-off, DC2_G214_CAPTRACE=1): per-present-tick triangle counts for the
// individual head sub-mesh texture pages, so the "head present but cap missing" window the
// user observed and the flicker's temporal shape (rapid per-frame vs contiguous stretch) are
// directly visible. Each page is an independent VU1 batch; G213 showed cap(0x35a0)+face(0x3420)
// drop as whole batches (0 tris) upstream of the GS. Prints one line per tick that touches the
// fbp=0x139 character RTT.
static bool g214CapTraceEnabled()
{
    static const bool e = envFlagEnabled("DC2_G214_CAPTRACE");
    return e;
}
static std::mutex g_g214Mutex;
static uint64_t g_g214Tick = 0;
// pages: cap 0x35a0, face 0x3420, hair 0x34c0, head-base 0x3480, body 0x3460, neck-decal 0x34a0
static uint64_t g_g214Cap = 0, g_g214Face = 0, g_g214Hair = 0, g_g214Head = 0, g_g214Body = 0, g_g214Neck = 0;
static void g214CapFlushLocked(uint64_t newTick)
{
    if (g_g214Tick != 0 && (g_g214Cap | g_g214Face | g_g214Hair | g_g214Head | g_g214Body | g_g214Neck))
    {
        std::fprintf(stderr,
            "[G214:cap] tick=%llu cap35a0=%llu face3420=%llu hair34c0=%llu head3480=%llu body3460=%llu neck34a0=%llu\n",
            (unsigned long long)g_g214Tick,
            (unsigned long long)g_g214Cap, (unsigned long long)g_g214Face,
            (unsigned long long)g_g214Hair, (unsigned long long)g_g214Head,
            (unsigned long long)g_g214Body, (unsigned long long)g_g214Neck);
    }
    g_g214Tick = newTick;
    g_g214Cap = g_g214Face = g_g214Hair = g_g214Head = g_g214Body = g_g214Neck = 0;
}
