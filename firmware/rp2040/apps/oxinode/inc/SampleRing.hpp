#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Lock-free single-producer / single-consumer ring buffer.
//
// Producer = core1 sample-drain context (decodes the chip FIFO).
// Consumer = core0 main loop (DSP read-out + JSON encode + USB enqueue).
//
// The two cores never write to the same field. The producer owns
// `m_head` and the slots between `m_tail` and `m_head`; the consumer
// owns `m_tail` and the slots between `m_tail` and `m_head` for read.
// Synchronisation is by acquire/release on the two indices — no
// `__disable_irq`, no `mutex_t`, no critical section. The classic
// Lamport-style SPSC.
//
// Heads and tails are *monotonically increasing* `uint32_t` counters,
// not modulo `Capacity`. Modular subtraction in u32 still gives the
// right depth (wraps cleanly at 2^32 ≈ 6 years of pushes at 25 Hz —
// fine). Slot index uses bit-mask, so `Capacity` MUST be a power of 2.

namespace oxinode::rp2040
{
    template <std::size_t Capacity>
    class SampleRing
    {
    public:
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "SampleRing Capacity must be a power of 2");
        static_assert(Capacity >= 4 && Capacity <= 4096,
                      "SampleRing Capacity outside the sane range");

        struct Sample
        {
            std::uint32_t tMs;
            std::uint32_t ir;
            std::uint32_t red;
        };

        SampleRing() = default;
        SampleRing(const SampleRing&)            = delete;
        SampleRing& operator=(const SampleRing&) = delete;

        // ── Producer side ──────────────────────────────────────────
        // Returns true on success, false if the ring is full. On
        // success the sample lives in slot[head], and the public
        // `head` advances by one. Safe ONLY from a single producer
        // context — concurrent producers will lose pushes.
        [[nodiscard]] bool tryPush(const Sample& s) noexcept
        {
            const std::uint32_t head = m_head.load(std::memory_order_relaxed);
            const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
            if ((head - tail) >= Capacity)
            {
                return false;   // full — caller decides whether to count drops
            }
            m_buf[head & kMask] = s;
            // Release pairs with consumer's acquire on m_head; ensures
            // the slot write is visible before the index advance.
            m_head.store(head + 1, std::memory_order_release);
            return true;
        }

        // ── Consumer side ──────────────────────────────────────────
        // Returns true on success, false if empty. On success, fills
        // `out` and the public `tail` advances by one.
        [[nodiscard]] bool tryPop(Sample& out) noexcept
        {
            const std::uint32_t tail = m_tail.load(std::memory_order_relaxed);
            const std::uint32_t head = m_head.load(std::memory_order_acquire);
            if (head == tail)
            {
                return false;
            }
            out = m_buf[tail & kMask];
            m_tail.store(tail + 1, std::memory_order_release);
            return true;
        }

        // ── Diagnostics (any context) ──────────────────────────────
        // Current depth — for high-watermark tracking. Reads both
        // indices with acquire so callers see a consistent snapshot.
        [[nodiscard]] std::uint32_t depth() const noexcept
        {
            const std::uint32_t head = m_head.load(std::memory_order_acquire);
            const std::uint32_t tail = m_tail.load(std::memory_order_acquire);
            return head - tail;
        }

        static constexpr std::size_t capacity() noexcept { return Capacity; }

    private:
        static constexpr std::uint32_t kMask =
            static_cast<std::uint32_t>(Capacity - 1);

        Sample m_buf[Capacity] = {};
        std::atomic<std::uint32_t> m_head{0};
        std::atomic<std::uint32_t> m_tail{0};
    };
}
