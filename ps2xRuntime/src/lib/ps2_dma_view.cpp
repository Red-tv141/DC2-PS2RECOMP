#include "ps2_dma_view.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <thread>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

namespace ps2_dma
{
namespace
{
std::mutex registryMutex;
std::array<ArenaState*, 16> registry{};
Counters stats;
#if defined(_WIN32)
LONG CALLBACK preserveFault(PEXCEPTION_POINTERS exception);
void* faultHandler = nullptr;
#endif
}
Counters& counters() { return stats; }

struct ArenaState
{
    uint8_t* memory = nullptr;
    size_t byteCount = 0;
    std::recursive_mutex mutex;
    std::vector<std::weak_ptr<PageGeneration>> current;
    std::vector<PageGeneration*> identity;
    bool batchRetire = false;
    std::vector<size_t> retired;

    void protect(size_t first, size_t count, bool readOnly)
    {
        if (!count) return;
#if defined(_WIN32)
        DWORD old;
        if (!VirtualProtect(memory + first * pageSize, count * pageSize,
                            readOnly ? PAGE_READONLY : PAGE_READWRITE, &old))
            std::abort(); // Never publish a view whose writes are unprotected.
#else
        if (mprotect(memory + first * pageSize, count * pageSize,
                     readOnly ? PROT_READ : (PROT_READ | PROT_WRITE))) std::abort();
#endif
    }

    bool preserve(size_t page)
    {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if (page >= current.size()) return false;
        // Another writer or the consumer may have retired the protection while
        // this fault was waiting for the arena mutex. Retry the now-writable
        // instruction; it is not an unrelated access violation.
        if (!identity[page]) return true;
        auto generation = current[page].lock();
        if (generation)
        {
            std::unique_lock<std::shared_mutex> pageGuard(generation->mutex);
            // Allocation is outside the guest arena. No guest/renderer call is
            // made from this handler, so it cannot wait on the submitting EE.
            generation->snapshot = std::make_unique<uint8_t[]>(pageSize);
            std::memcpy(generation->snapshot.get(), memory + page * pageSize, pageSize);
            generation->bytes = generation->snapshot.get();
            stats.snapshotBytes.fetch_add(pageSize, std::memory_order_relaxed);
        }
        current[page].reset();
        identity[page] = nullptr;
        if (batchRetire) retired.push_back(page);
        else protect(page, 1, false);
        return true;
    }

    void retire(size_t page, PageGeneration* generation)
    {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if (identity[page] != generation) return;
        current[page].reset();
        identity[page] = nullptr;
        if (batchRetire) retired.push_back(page);
        else protect(page, 1, false);
    }

    ~ArenaState()
    {
        std::lock_guard<std::mutex> guard(registryMutex);
        for (auto& entry : registry) if (entry == this) entry = nullptr;
#if defined(_WIN32)
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
        if (faultHandler && std::all_of(registry.begin(), registry.end(), [](auto p) { return !p; }))
        { RemoveVectoredExceptionHandler(faultHandler); faultHandler = nullptr; }
#else
        if (memory) munmap(memory, byteCount);
#endif
    }
};

namespace
{
#if defined(_WIN32)
LONG CALLBACK preserveFault(PEXCEPTION_POINTERS exception)
{
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t address = record->ExceptionInformation[1];
    std::lock_guard<std::mutex> guard(registryMutex);
    for (auto* arena : registry)
        if (arena && address >= uintptr_t(arena->memory) &&
            address - uintptr_t(arena->memory) < arena->byteCount)
            return arena->preserve((address - uintptr_t(arena->memory)) / pageSize)
                ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}

PageGeneration::~PageGeneration() { arena->retire(page, this); }
void prepareWrite(void* destination, size_t bytes)
{
    if (!bytes) return;
    const uintptr_t address = uintptr_t(destination);
    std::lock_guard<std::mutex> guard(registryMutex);
    for (auto* arena : registry)
    {
        if (!arena || address < uintptr_t(arena->memory) ||
            address - uintptr_t(arena->memory) >= arena->byteCount) continue;
        const size_t offset = address - uintptr_t(arena->memory);
        const size_t length = std::min(bytes, arena->byteCount - offset);
        const size_t first = offset / pageSize, last = (offset + length - 1) / pageSize;
        for (size_t page = first; page <= last; ++page) arena->preserve(page);
        return;
    }
}
Arena::Arena(size_t bytes) : state(std::make_shared<ArenaState>())
{
    if (!bytes || bytes > SIZE_MAX - pageSize) throw std::invalid_argument("DMA arena size");
    state->byteCount = (bytes + pageSize - 1) & ~(pageSize - 1);
#if defined(_WIN32)
    state->memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, state->byteCount,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!state->memory) throw std::bad_alloc();
#else
    auto ptr = mmap(nullptr, state->byteCount, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) throw std::bad_alloc();
    state->memory = static_cast<uint8_t*>(ptr);
#endif
    state->current.resize(state->byteCount / pageSize);
    state->identity.resize(state->current.size());
    std::lock_guard<std::mutex> guard(registryMutex);
    auto slot = std::find(registry.begin(), registry.end(), nullptr);
    if (slot == registry.end()) throw std::runtime_error("Too many guest DMA arenas");
#if defined(_WIN32)
    if (!faultHandler)
    {
        faultHandler = AddVectoredExceptionHandler(1, preserveFault);
        if (!faultHandler) throw std::runtime_error("DMA write-fault handler unavailable");
    }
#endif
    *slot = state.get();
}
Arena::~Arena() = default;
uint8_t* Arena::data() const { return state->memory; }
size_t Arena::size() const { return state->byteCount; }

std::vector<std::shared_ptr<PageGeneration>> Arena::pin(size_t offset, size_t bytes)
{
    if (offset > size() || bytes > size() - offset) throw std::out_of_range("DMA pin range");
    std::vector<std::shared_ptr<PageGeneration>> pages;
    if (!bytes) return pages;
    size_t first = offset / pageSize, last = (offset + bytes - 1) / pageSize;
    pages.reserve(last - first + 1);
#if !defined(_WIN32)
    // Platforms without the write-fault backend retain exact snapshot semantics.
    // The runtime keeps the existing copied-chain arm on those platforms.
    for (size_t page = first; page <= last; ++page)
    {
        auto generation = std::make_shared<PageGeneration>();
        generation->arena = state; generation->page = page;
        generation->snapshot = std::make_unique<uint8_t[]>(pageSize);
        std::memcpy(generation->snapshot.get(), state->memory + page * pageSize, pageSize);
        generation->bytes = generation->snapshot.get();
        pages.push_back(std::move(generation));
    }
    return pages;
#endif
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    size_t runFirst = first, runCount = 0;
    for (size_t page = first; page <= last; ++page)
    {
        auto generation = state->current[page].lock();
        if (!generation)
        {
            generation = std::make_shared<PageGeneration>();
            generation->arena = state;
            generation->page = page;
            generation->bytes = state->memory + page * pageSize;
            state->current[page] = generation;
            state->identity[page] = generation.get();
            if (!runCount) runFirst = page;
            ++runCount;
        }
        else if (runCount)
        { state->protect(runFirst, runCount, true); runCount = 0; }
        pages.push_back(std::move(generation));
    }
    state->protect(runFirst, runCount, true);
    stats.referencedBytes.fetch_add(bytes, std::memory_order_relaxed);
    stats.referencedPages.fetch_add(pages.size(), std::memory_order_relaxed);
    return pages;
}

std::shared_ptr<PageGeneration> Arena::pinPage(size_t offset, size_t bytes)
{
    if (!bytes || offset > size() || bytes > size() - offset ||
        (offset & (pageSize - 1)) + bytes > pageSize) throw std::out_of_range("DMA single-page range");
#if !defined(_WIN32)
    return pin(offset, bytes).front();
#else
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    const size_t page = offset / pageSize;
    auto generation = state->current[page].lock();
    if (!generation)
    {
        generation = std::make_shared<PageGeneration>();
        generation->arena = state; generation->page = page;
        generation->bytes = state->memory + page * pageSize;
        state->current[page] = generation; state->identity[page] = generation.get();
        state->protect(page, 1, true);
    }
    stats.referencedBytes.fetch_add(bytes, std::memory_order_relaxed);
    stats.referencedPages.fetch_add(1, std::memory_order_relaxed);
    return generation;
#endif
}

namespace
{
struct DescriptorPool
{
    std::mutex mutex;
    std::array<std::vector<Fragment>, 8> buffers;
    size_t count = 0;
};
DescriptorPool descriptorPool;
}
Stream::Stream()
{
    std::lock_guard<std::mutex> guard(descriptorPool.mutex);
    if (descriptorPool.count) fragments = std::move(descriptorPool.buffers[--descriptorPool.count]);
}
void Stream::append(Arena& arena, size_t offset, size_t bytes)
{
    if (!bytes) return;
    auto appendPage = [&](std::shared_ptr<PageGeneration> generation)
    {
        size_t inPage = offset & (pageSize - 1);
        size_t chunk = std::min(bytes, pageSize - inPage);
        // Adjacent tags may reference adjacent bytes of the same generation.
        if (!fragments.empty() && fragments.back().generation == generation &&
            fragments.back().pageOffset + fragments.back().bytes == inPage)
            fragments.back().bytes += uint32_t(chunk);
        else
            fragments.push_back({std::move(generation), uint32_t(inPage), uint32_t(chunk), byteCount});
        byteCount += chunk; offset += chunk; bytes -= chunk;
    };
    if ((offset & (pageSize - 1)) + bytes <= pageSize)
        appendPage(arena.pinPage(offset, bytes));
    else
    {
        auto pages = arena.pin(offset, bytes);
        if (fragments.size() + pages.size() > fragments.capacity())
            fragments.reserve(std::max(fragments.size() + pages.size(), fragments.capacity() * 2 + 256));
        for (auto& generation : pages) appendPage(std::move(generation));
    }
}

namespace
{
void retireFragments(std::vector<Fragment>& fragments, size_t firstSpan, size_t lastSpan)
{
    // VirtualProtect is a system call. Retire contiguous page runs once, not
    // once per 4 KiB descriptor. Keep arenas alive until all protections retire.
    std::vector<std::shared_ptr<ArenaState>> arenas;
    for (size_t i = firstSpan; i < lastSpan; ++i)
    {
        const auto& span = fragments[i];
        if (span.generation && std::find(arenas.begin(), arenas.end(), span.generation->arena) == arenas.end())
            arenas.push_back(span.generation->arena);
    }
    for (const auto& arena : arenas)
    {
        std::lock_guard<std::recursive_mutex> guard(arena->mutex);
        arena->retired.reserve(lastSpan - firstSpan);
        arena->batchRetire = true;
        for (size_t i = firstSpan; i < lastSpan; ++i)
        {
            auto& span = fragments[i];
            if (span.generation && span.generation->arena == arena) span.generation.reset();
        }
        arena->batchRetire = false;
        std::sort(arena->retired.begin(), arena->retired.end());
        for (size_t first = 0; first < arena->retired.size();)
        {
            size_t end = first + 1;
            while (end < arena->retired.size() && arena->retired[end] == arena->retired[end - 1] + 1) ++end;
            arena->protect(arena->retired[first], end - first, false);
            first = end;
        }
        arena->retired.clear();
    }
}
}
Stream::~Stream()
{
    retireFragments(fragments, retired, fragments.size());
    fragments.clear();
    std::lock_guard<std::mutex> guard(descriptorPool.mutex);
    if (descriptorPool.count < descriptorPool.buffers.size())
        descriptorPool.buffers[descriptorPool.count++] = std::move(fragments);
}
void Stream::retireBefore(size_t offset) const
{
    // Release consumed ownership in coarse batches: an EE reuse of an already
    // consumed prefix needs no snapshot. Repeated references keep their shared
    // generation alive until their final occurrence has passed.
    if (retired == fragments.size() || offset < fragments[retired].streamOffset ||
        (offset != size() && offset - fragments[retired].streamOffset < 256 * 1024)) return;
    size_t end = retired;
    while (end < fragments.size() && fragments[end].streamOffset + fragments[end].bytes <= offset) ++end;
    retireFragments(fragments, retired, end); retired = end;
}

void Stream::verifyReference() const
{
    if (reference.empty()) return;
    if (reference.size() != size()) throw std::runtime_error("DMA reference size mismatch");
    Input cursor(*this);
    std::array<uint8_t, 4096> bytes;
    for (size_t offset = 0; offset < size(); offset += bytes.size())
    {
        size_t count = std::min(bytes.size(), size() - offset);
        cursor.copy(bytes.data(), offset, count);
        if (std::memcmp(bytes.data(), reference.data() + offset, count))
            throw std::runtime_error("DMA copied-chain oracle mismatch");
    }
}

void Input::release()
{
    if (lock.owns_lock()) lock.unlock();
    base = nullptr;
}
void Input::select(size_t offset)
{
    if (offset >= source.size()) throw std::out_of_range("DMA stream read");
    const auto& spans = source.spans();
    if (base && offset >= begin && offset < end) return;
    release();
    if (index + 1 < spans.size() && offset >= spans[index + 1].streamOffset &&
        offset < spans[index + 1].streamOffset + spans[index + 1].bytes) ++index;
    else if (index == size_t(-1) || offset < begin || offset >= end)
    {
        auto it = std::upper_bound(spans.begin(), spans.end(), offset,
            [](size_t v, const Fragment& f) { return v < f.streamOffset; });
        index = size_t((it - spans.begin()) - 1);
    }
    const auto& span = spans[index];
    if (!span.generation) throw std::logic_error("DMA reader revisited a consumed prefix");
    lock = std::shared_lock<std::shared_mutex>(span.generation->mutex);
    begin = span.streamOffset; end = begin + span.bytes;
    base = span.generation->bytes + span.pageOffset;
}
void Input::copy(void* dst, size_t offset, size_t bytes)
{
    if (offset > source.size() || bytes > source.size() - offset) throw std::out_of_range("DMA copy range");
    auto* out = static_cast<uint8_t*>(dst);
    while (bytes)
    {
        select(offset);
        size_t chunk = std::min(bytes, end - offset);
        std::memcpy(out, base + offset - begin, chunk);
        out += chunk; offset += chunk; bytes -= chunk;
    }
}
const uint8_t* Input::borrow(size_t offset, size_t bytes)
{
    if (offset > source.size() || bytes > source.size() - offset) throw std::out_of_range("DMA borrow range");
    if (!bytes) return nullptr;
    select(offset);
    if (bytes <= end - offset) return base + offset - begin;
    scratch.resize(bytes);
    copy(scratch.data(), offset, bytes);
    stats.boundaryBytes.fetch_add(bytes, std::memory_order_relaxed);
    return scratch.data();
}

struct Transport::State
{
    static constexpr uint64_t capacity = 4;
    std::array<std::unique_ptr<Stream>, capacity> ring;
    alignas(64) std::atomic<uint64_t> published{0};
    alignas(64) std::atomic<uint64_t> consumed{0};
    std::mutex mutex;
    std::condition_variable wake, progress;
    std::atomic<bool> stop{false};
    std::exception_ptr error;
    Consume consume;
    std::thread worker;
    explicit State(Consume fn) : consume(std::move(fn)), worker([this] { run(); }) {}
    void run()
    {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), L"DMA VIF1 consumer");
#endif
        for (;;)
        {
            auto read = consumed.load(std::memory_order_relaxed);
            if (read == published.load(std::memory_order_acquire))
            {
                std::unique_lock<std::mutex> guard(mutex);
                wake.wait(guard, [&] { return stop.load() ||
                    consumed.load() != published.load(); });
                if (stop.load() && consumed.load() == published.load()) return;
                continue;
            }
            auto stream = std::move(ring[read % capacity]);
            auto completion = stream->takeCompletion();
            bool succeeded = false;
            try { consume(*stream); succeeded = true; }
            catch (...) { std::lock_guard<std::mutex> guard(mutex); error = std::current_exception(); }
            stream.reset(); // Retire pins before advertising DMA completion.
            {
                std::lock_guard<std::mutex> guard(mutex);
                consumed.store(read + 1, std::memory_order_release);
            }
            stats.completed.fetch_add(1, std::memory_order_relaxed);
            try { if (succeeded && completion) completion(); }
            catch (...) { std::lock_guard<std::mutex> guard(mutex); error = std::current_exception(); }
            progress.notify_all(); // Every observer of this completed epoch may proceed.
        }
    }
};
Transport::Transport(Consume consume) : state(std::make_unique<State>(std::move(consume))) {}
Transport::~Transport()
{
    // A teardown must retire every borrowed guest page even after parser failure.
    try { fence(); } catch (...) {}
    { std::lock_guard<std::mutex> guard(state->mutex); state->stop.store(true); }
    state->wake.notify_one();
    state->worker.join();
}
void Transport::submit(std::unique_ptr<Stream> stream)
{
    auto write = state->published.load(std::memory_order_relaxed);
    auto read = state->consumed.load(std::memory_order_acquire);
    if (write - read == State::capacity)
    {
        std::unique_lock<std::mutex> guard(state->mutex);
        state->progress.wait(guard, [&] { return write - state->consumed.load() < State::capacity; });
        if (state->error) std::rethrow_exception(state->error);
        read = state->consumed.load(std::memory_order_acquire);
    }
    state->ring[write % State::capacity] = std::move(stream);
    // The mutex prevents lost notifications at the sleep/publication transition;
    // it is taken once per chain, never per word/page/command.
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->published.store(write + 1, std::memory_order_release);
    }
    state->wake.notify_one();
    stats.submitted.fetch_add(1, std::memory_order_relaxed);
    auto lead = write + 1 - read, maximum = stats.maxLead.load(std::memory_order_relaxed);
    while (lead > maximum && !stats.maxLead.compare_exchange_weak(maximum, lead, std::memory_order_relaxed)) {}
    static const bool census = [] { const char* v = std::getenv("DC2_G772_DMA_STAT"); return v && std::atoi(v); }();
    if (census && ((write + 1) % 128 == 0))
        std::fprintf(stderr, "[G772:dma] submits=%llu complete=%llu refBytes=%llu pages=%llu cowBytes=%llu boundaryBytes=%llu lead=%llu maxLead=%llu\n",
            (unsigned long long)stats.submitted.load(), (unsigned long long)stats.completed.load(),
            (unsigned long long)stats.referencedBytes.load(), (unsigned long long)stats.referencedPages.load(),
            (unsigned long long)stats.snapshotBytes.load(), (unsigned long long)stats.boundaryBytes.load(),
            (unsigned long long)lead, (unsigned long long)stats.maxLead.load());
}
void Transport::fence()
{
    if (std::this_thread::get_id() == state->worker.get_id()) return;
    auto epoch = state->published.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> guard(state->mutex);
    state->progress.wait(guard, [&] { return state->consumed.load(std::memory_order_acquire) >= epoch; });
    if (state->error) std::rethrow_exception(state->error);
}
bool Transport::ready() const
{
    return state->consumed.load(std::memory_order_acquire) >=
           state->published.load(std::memory_order_acquire);
}
}
