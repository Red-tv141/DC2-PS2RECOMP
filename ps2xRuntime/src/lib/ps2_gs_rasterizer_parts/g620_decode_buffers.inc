// ===== G620: the decoded-texel buffer POOL =====
//
// WHAT IT DELETES.  `[G620:decstage]` split the flush's texture-decode bracket on `dungeon1` and
// found that **the decode is a minority of it**:
//
//     msPerFlush(resize=0.0173  decode=0.0174  replace=0.0115)   x45 flushes/f
//     =>          resize 0.78 ms/f   decode 0.78 ms/f   replace 0.52 ms/f
//
// `resize` is `up.px.resize(w*h)` on a `std::vector<uint32_t>` that was just default-constructed.
// `std::vector::resize` VALUE-INITIALISES the new elements, so every decode paid a fresh
// allocation plus a zero-fill of up to 1 MiB — and `g178DecodeWhole` then overwrites **every one
// of those texels** (`GSRowPool::run(0, texH - 1, ...)` covers every row and each row writes
// `x` in `[0, texW)`), so the zero-fill's result is never observed by anything.
//
// The storage churn was already half-noticed by the backend: `lle_gpu_raster_backend.inc` hands the
// completed batch's `texUploads` vector BACK to the front end (`m_asyncRecycle`) precisely so its
// capacity can be reused — and the front end then called `texUploads.clear()`, which destroys every
// element and frees exactly the buffers the recycle just returned.
//
// MECHANISM.  A small pool of `std::vector<uint32_t>`, owned by the GS worker (the only thread that
// walks this pass, exactly like `g_g178TexReg`). `clear()` becomes "recycle each `px` into the
// pool, then clear"; the decode asks the pool for a buffer.
//
//   * an EXACT-SIZE hit costs neither an allocation nor a fill — the buffer already has `size()==n`
//     and its stale texels are overwritten in full before anything reads them;
//   * otherwise the largest-capacity pooled buffer is resized, which still avoids the allocation
//     when its capacity suffices and value-initialises only the size delta;
//   * an empty pool falls through to exactly the previous behaviour.
//
// EXACTNESS.  The property required is "`g178DecodeWhole` writes all `w*h` texels before any read".
// That is structural (see above), not statistical — but it is also the one thing a future format
// arm could break silently, so it has its own oracle: `DC2_G620_PXPOOL_POISON=1` fills each
// acquired buffer with `0xDEADBEEF` and counts how many texels still carry it after the decode.
// Rollback: `DC2_G620_NO_PXPOOL=1` restores the per-decode construct/resize/destroy cycle.

constexpr size_t kG620PxPoolMax = 24u;
constexpr uint32_t kG620PxPoison = 0xDEADBEEFu;

static const bool g_g620NoPxPool = (std::getenv("DC2_G620_NO_PXPOOL") != nullptr);
static const bool g_g620PxPoison = (std::getenv("DC2_G620_PXPOOL_POISON") != nullptr);

static std::vector<std::vector<uint32_t>> g_g620PxPool;
static uint64_t g_g620PxExact = 0, g_g620PxGrow = 0, g_g620PxFresh = 0;
static uint64_t g_g620PoisonChecked = 0, g_g620PoisonLeft = 0;

static std::vector<uint32_t> g620AcquirePx(size_t n)
{
    if (!g_g620NoPxPool)
    {
        for (size_t i = 0; i < g_g620PxPool.size(); ++i)
        {
            if (g_g620PxPool[i].size() != n)
                continue;
            std::vector<uint32_t> v = std::move(g_g620PxPool[i]);
            g_g620PxPool.erase(g_g620PxPool.begin() + static_cast<ptrdiff_t>(i));
            ++g_g620PxExact;
            if (g_g620PxPoison)
                std::fill(v.begin(), v.end(), kG620PxPoison);
            return v; // exact size already: no allocation and no value-initialisation
        }
        if (!g_g620PxPool.empty())
        {
            size_t best = 0;
            for (size_t i = 1; i < g_g620PxPool.size(); ++i)
                if (g_g620PxPool[i].capacity() > g_g620PxPool[best].capacity())
                    best = i;
            std::vector<uint32_t> v = std::move(g_g620PxPool[best]);
            g_g620PxPool.erase(g_g620PxPool.begin() + static_cast<ptrdiff_t>(best));
            v.resize(n);
            ++g_g620PxGrow;
            if (g_g620PxPoison)
                std::fill(v.begin(), v.end(), kG620PxPoison);
            return v;
        }
    }
    ++g_g620PxFresh;
    std::vector<uint32_t> v;
    v.resize(n);
    if (g_g620PxPoison)
        std::fill(v.begin(), v.end(), kG620PxPoison);
    return v;
}

static void g620RecyclePx(std::vector<uint32_t> &&v)
{
    if (g_g620NoPxPool || v.capacity() == 0u)
        return;
    if (g_g620PxPool.size() >= kG620PxPoolMax)
    {
        size_t worst = 0;
        for (size_t i = 1; i < g_g620PxPool.size(); ++i)
            if (g_g620PxPool[i].capacity() < g_g620PxPool[worst].capacity())
                worst = i;
        if (g_g620PxPool[worst].capacity() >= v.capacity())
            return; // the pool already holds a strictly better buffer for every future request
        g_g620PxPool.erase(g_g620PxPool.begin() + static_cast<ptrdiff_t>(worst));
    }
    g_g620PxPool.push_back(std::move(v));
}

// The pool's exactness oracle. ⛔ `left=0` is only a result when `checked` is large (G589).
static void g620PoisonCheck(const std::vector<uint32_t> &px, size_t n)
{
    if (!g_g620PxPoison)
        return;
    const size_t lim = n < px.size() ? n : px.size();
    for (size_t i = 0; i < lim; ++i)
        if (px[i] == kG620PxPoison)
            ++g_g620PoisonLeft;
    g_g620PoisonChecked += lim;
}
