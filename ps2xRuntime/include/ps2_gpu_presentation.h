#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// A frozen presentation image, not an alias of a live GS render target. The
// producer may recycle storage only when the pool is its sole CPU owner, and
// after waiting on the consumer's GPU fence. No guest-visible memory lives here.
namespace dc2::gpu_present
{
struct Frame
{
    uint32_t texture = 0, width = 0, height = 0;
    void* ready = nullptr;
    void* consumed = nullptr;
    std::mutex mutex;
};
using FramePtr = std::shared_ptr<Frame>;

bool enabled();
// Called while the main context is released, before the backend starts.
void initialize(void* deviceContext, void* mainContext);
// Backend GL owner only. Failure/exhaustion preserves the existing CPU path.
FramePtr begin(uint32_t width, uint32_t height);
bool publish(const FramePtr& frame);
// Presentation GL owner only. Keep the lease until finishRead has fenced every
// submitted sample. Repeated reads from this one context replace its older fence.
bool waitReady(const FramePtr& frame);
void finishRead(const FramePtr& frame);
// Exact, demand-driven CPU API. Uses an independent shared GL context, so a
// paused GS worker or a frame-dump caller cannot deadlock waiting on its queue.
bool readPixels(const FramePtr& frame, std::vector<uint8_t>& rgba);
}
