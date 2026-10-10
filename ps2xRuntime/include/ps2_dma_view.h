#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <vector>

// Guest pages have immutable DMA generations. The guest address never changes;
// a write fault preserves the old generation before making that page writable.
// Every pointer borrowed by a consumer owns a shared page lock. A writer cannot
// resume until that borrow has ended, even if the consumer was already active.
namespace ps2_dma
{
constexpr size_t pageSize = 4096;
struct ArenaState;
struct PageGeneration
{
    std::shared_ptr<ArenaState> arena;
    size_t page = 0;
    mutable std::shared_mutex mutex;
    const uint8_t* bytes = nullptr;
    std::unique_ptr<uint8_t[]> snapshot;
    ~PageGeneration();
};

struct Counters
{
    std::atomic<uint64_t> referencedBytes{0}, referencedPages{0};
    std::atomic<uint64_t> snapshotBytes{0}, boundaryBytes{0};
    std::atomic<uint64_t> submitted{0}, completed{0}, maxLead{0};
};
Counters& counters();
// Kernel/CRT IO can reject a read-only destination instead of raising a user
// write fault. Preserve pinned generations before handing guest RAM to it.
void prepareWrite(void* destination, size_t bytes);

class Arena
{
public:
    explicit Arena(size_t bytes);
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    uint8_t* data() const;
    size_t size() const;
    // Pinning is performed by the guest-memory producer, before publication.
    std::vector<std::shared_ptr<PageGeneration>> pin(size_t offset, size_t bytes);
    std::shared_ptr<PageGeneration> pinPage(size_t offset, size_t bytes);
private:
    std::shared_ptr<ArenaState> state;
};

struct Fragment
{
    std::shared_ptr<PageGeneration> generation;
    uint32_t pageOffset = 0, bytes = 0;
    size_t streamOffset = 0;
};

class Stream
{
public:
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    void append(Arena& arena, size_t offset, size_t bytes);
    size_t size() const { return byteCount; }
    bool empty() const { return byteCount == 0; }
    const std::vector<Fragment>& spans() const { return fragments; }
    void setReference(std::vector<uint8_t> bytes) { reference = std::move(bytes); }
    void verifyReference() const;
    void retireBefore(size_t offset) const;
    void setCompletion(std::function<void()> fn) { completion = std::move(fn); }
    std::function<void()> takeCompletion() { return std::move(completion); }
private:
    mutable std::vector<Fragment> fragments;
    mutable size_t retired = 0;
    size_t byteCount = 0;
    std::vector<uint8_t> reference;
    std::function<void()> completion;
};

class Input;
struct Address
{
    Input* input; size_t offset;
    Address operator+(size_t n) const { return {input, offset + n}; }
    Address operator-(size_t n) const { return {input, offset - n}; }
};

// Sequential reads stay in a cached span with no allocation, lookup, or atomic.
// Only a command that crosses a page/tag fragment is assembled in scratch.
class Input
{
public:
    explicit Input(const Stream& stream) : source(stream) {}
    Address operator+(size_t offset) { return {this, offset}; }
    uint8_t operator[](size_t offset) { uint8_t v; copy(&v, offset, 1); return v; }
    explicit operator bool() const { return !source.empty(); }
    void copy(void* dst, size_t offset, size_t bytes);
    const uint8_t* borrow(size_t offset, size_t bytes);
    size_t contiguous(size_t offset) { select(offset); return end - offset; }
    void release();
    void consumeThrough(size_t offset) { if (offset >= end) release(); source.retireBefore(offset); }
private:
    void select(size_t offset);
    const Stream& source;
    size_t index = size_t(-1), begin = 0, end = 0;
    const uint8_t* base = nullptr;
    std::shared_lock<std::shared_mutex> lock;
    std::vector<uint8_t> scratch;
};

inline void read(void* dst, const uint8_t* src, size_t bytes) { std::memcpy(dst, src, bytes); }
inline void read(void* dst, Address src, size_t bytes) { src.input->copy(dst, src.offset, bytes); }
inline void read(void* dst, Input& src, size_t bytes) { src.copy(dst, 0, bytes); }
inline const uint8_t* borrow(const uint8_t* src, size_t) { return src; }
inline const uint8_t* borrow(Address src, size_t bytes) { return src.input->borrow(src.offset, bytes); }
inline const uint8_t* borrow(Input& src, size_t bytes) { return src.borrow(0, bytes); }
inline size_t contiguous(const uint8_t*) { return SIZE_MAX; }
inline size_t contiguous(Address src) { return src.input->contiguous(src.offset); }
inline void release(const uint8_t*&) {}
inline void release(Input& src) { src.release(); }
inline void consumeThrough(const uint8_t*&, size_t) {}
inline void consumeThrough(Input& src, size_t offset) { src.consumeThrough(offset); }
inline uint32_t guestAddress(const uint8_t* src, const uint8_t* ram)
{ return ram ? uint32_t((uintptr_t(src) - uintptr_t(ram)) & 0x1FFFFFFFu) : 0; }
inline uint32_t guestAddress(Address, const uint8_t*) { return 0; }

// Bounded SPSC publication. The worker exclusively owns VIF state; observers
// fence its epoch, rather than imposing a downstream GS/GPU completion wait.
class Transport
{
public:
    using Consume = std::function<void(const Stream&)>;
    explicit Transport(Consume consume);
    ~Transport();
    void submit(std::unique_ptr<Stream> stream);
    void fence();
    bool ready() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
