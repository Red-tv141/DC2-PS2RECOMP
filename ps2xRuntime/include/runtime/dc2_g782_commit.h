#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace dc2_g782 {

enum class Decision : uint8_t {
    Pending,
    NativeCommitted,
    LegacyCommitted,
};

// One token belongs to one logical E91F00 draw, never to an individual kick.
//
// Publication contract (G782):
//   G150  builds the immutable item, reserves an unpublished G713 ring slot and
//         fills it, THEN tryNative(). A winning release CAS is the transaction
//         boundary: before it native may still reject to legacy; after it the
//         ordered G713 node is guaranteed to be published and executed, and the
//         executor has no normal refusal left (plans/phase-G782-ledger.md).
//   MTVU  firstCore() at the first target core job: one acquire load, and if
//         still Pending one CAS Pending -> LegacyCommitted. It never waits.
//         Its acquire pairs with G150's release, so a NativeCommitted read
//         happens-after the item construction and the slot fill.
//   G713  the ring's own release/acquire publishes the node; the executor
//         re-reads the decision and draws only on NativeCommitted.
// Exactly one of tryNative()/reject()/firstCore()'s CAS can move the state out
// of Pending, so native and legacy can never both run and never both be absent.
class CommitToken {
public:
    explicit CommitToken(uint64_t drawId) : m_drawId(drawId) {}

    CommitToken(const CommitToken&) = delete;
    CommitToken& operator=(const CommitToken&) = delete;

    uint64_t drawId() const noexcept { return m_drawId; }

    Decision decision() const noexcept {
        return m_decision.load(std::memory_order_acquire);
    }

    bool tryNative() noexcept {
        Decision expected = Decision::Pending;
        return m_decision.compare_exchange_strong(
            expected, Decision::NativeCommitted,
            std::memory_order_release, std::memory_order_acquire);
    }

    bool reject() noexcept {
        Decision expected = Decision::Pending;
        return m_decision.compare_exchange_strong(
            expected, Decision::LegacyCommitted,
            std::memory_order_release, std::memory_order_acquire);
    }

    // Called once, immediately before the first target MTVU core. Never waits.
    Decision firstCore(bool *legacyWonPending = nullptr) noexcept {
        if (legacyWonPending) *legacyWonPending = false;
        if (m_firstCore.exchange(true, std::memory_order_acq_rel))
            fail("first core entered twice");
        Decision result = m_decision.load(std::memory_order_acquire);
        if (result == Decision::Pending) {
            Decision expected = Decision::Pending;
            if (m_decision.compare_exchange_strong(
                    expected, Decision::LegacyCommitted,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                result = Decision::LegacyCommitted;
                if (legacyWonPending) *legacyWonPending = true;
            } else
                result = expected;
        }
        if (result == Decision::Pending) fail("first core left the draw pending");
        m_groupDecision.store(result, std::memory_order_release);
        return result;
    }

    // Same MTVU worker owns all 13 target jobs in issue order. A mixed group
    // (start native / resume legacy or the reverse) is a fatal defect.
    Decision laterCore() const noexcept {
        const Decision group = m_groupDecision.load(std::memory_order_acquire);
        if (group == Decision::Pending) fail("resume before first core");
        if (m_decision.load(std::memory_order_acquire) != group) fail("mixed 13-kick group");
        return group;
    }

private:
    [[noreturn]] static void fail(const char *why) noexcept {
        std::fprintf(stderr, "[G782:token] FATAL %s\n", why);
        std::fflush(stderr);
        std::abort();
    }

    const uint64_t m_drawId;
    std::atomic<Decision> m_decision{Decision::Pending};
    std::atomic<Decision> m_groupDecision{Decision::Pending};
    std::atomic<bool> m_firstCore{false};
};

} // namespace dc2_g782
