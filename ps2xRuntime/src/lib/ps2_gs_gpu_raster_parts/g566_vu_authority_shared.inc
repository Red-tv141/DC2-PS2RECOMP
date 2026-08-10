#ifndef DC2_G566_VU_AUTHORITY_SHARED_INC
#define DC2_G566_VU_AUTHORITY_SHARED_INC

#include <atomic>
#include <condition_variable>
#include <mutex>

// G566 private cross-TU result. The GPU backend owns publication; the MTVU/MTGS side owns
// consumption. Keeping this out of the public runtime headers avoids rebuilding generated game
// code and preserves the frozen interface contract.
struct G566VuAuthorityResult
{
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> ready{false};
    bool ok = false;
    uint64_t sequence = 0ull;
    std::vector<std::vector<uint8_t>> packets;
};

#endif
