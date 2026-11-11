#pragma once

#include <atomic>
#include <cstdint>

/** SpscQueue : a small queue for passing messages from ONE context that
 *  writes (Push) to ONE context that reads (Pop), with no locking.
 *  Used for MIDI clock messages (MIDI code -> audio callback) and button
 *  commands (UI code -> audio callback). Holds N-1 items.
 *  Plain C++ so the Mac tests can use it. */
template <typename T, uint32_t N>
class SpscQueue
{
  public:
    /** Returns false (and counts a drop) if the queue is full. */
    bool Push(const T &item)
    {
        const uint32_t w    = write_.load(std::memory_order_relaxed);
        const uint32_t next = (w + 1) % N;
        if(next == read_.load(std::memory_order_acquire))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    /** Returns false if nothing is waiting. */
    bool Pop(T &item)
    {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if(r == write_.load(std::memory_order_acquire))
            return false;
        item = buf_[r];
        read_.store((r + 1) % N, std::memory_order_release);
        return true;
    }

    uint32_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

  private:
    T                     buf_[N];
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    std::atomic<uint32_t> dropped_{0};
};
