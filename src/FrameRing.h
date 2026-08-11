#pragma once

#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstddef>

// Single-producer / single-consumer ring buffer of interleaved audio frames.
//
// The invariant that matters: **every transfer moves whole frames**. An
// earlier version of this buffer counted individual samples and clamped
// transfers to the free/available sample count — which is odd half the time
// on a stereo stream. One truncated transfer left the buffer misaligned by a
// single sample, and from then on every read returned (R,L) instead of (L,R):
// the channels appeared swapped at random, permanently, until restart.
// Counting frames makes that unrepresentable.
//
// Threading: Write() is called only by the producer; Read() and DropOldest()
// only by the consumer. Available() is safe from either side.
class FrameRing
{
public:
    void Reset(size_t capacityFrames, uint16_t channels)
    {
        capacityFrames_ = capacityFrames;
        channels_       = channels ? channels : 1;
        buf_.assign(capacityFrames_ * channels_, 0.0f);
        write_.store(0);
        read_.store(0);
    }

    void Release()
    {
        buf_.clear();
        buf_.shrink_to_fit();
        capacityFrames_ = 0;
        write_.store(0);
        read_.store(0);
    }

    size_t   Capacity() const { return capacityFrames_; }
    uint16_t Channels() const { return channels_; }

    size_t Available() const
    {
        if (capacityFrames_ == 0) return 0;
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return (w + capacityFrames_ - r) % capacityFrames_;
    }

    // Producer side. Returns the number of frames actually written; a short
    // write means the buffer was full (the consumer stalled).
    size_t Write(const float* src, size_t frames)
    {
        if (capacityFrames_ == 0 || frames == 0 || !src) return 0;
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t freeFrames = (r + capacityFrames_ - w - 1) % capacityFrames_;
        const size_t n = (std::min)(frames, freeFrames);
        if (n == 0) return 0;

        const size_t first = (std::min)(n, capacityFrames_ - w);
        std::memcpy(&buf_[w * channels_], src, first * channels_ * sizeof(float));
        if (n > first)
            std::memcpy(&buf_[0], src + first * channels_,
                        (n - first) * channels_ * sizeof(float));

        write_.store((w + n) % capacityFrames_, std::memory_order_release);
        return n;
    }

    // Consumer side. Returns frames read; a short read means an underrun and
    // the caller should pad the remainder with silence.
    size_t Read(float* dst, size_t frames)
    {
        if (capacityFrames_ == 0 || frames == 0 || !dst) return 0;
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t avail = (w + capacityFrames_ - r) % capacityFrames_;
        const size_t n = (std::min)(frames, avail);
        if (n == 0) return 0;

        const size_t first = (std::min)(n, capacityFrames_ - r);
        std::memcpy(dst, &buf_[r * channels_], first * channels_ * sizeof(float));
        if (n > first)
            std::memcpy(dst + first * channels_, &buf_[0],
                        (n - first) * channels_ * sizeof(float));

        read_.store((r + n) % capacityFrames_, std::memory_order_release);
        return n;
    }

    // Consumer side. Throws away the oldest frames — used to hold the queue
    // at the target latency when the two device clocks drift apart.
    void DropOldest(size_t frames)
    {
        if (capacityFrames_ == 0 || frames == 0) return;
        const size_t n = (std::min)(frames, Available());
        if (n == 0) return;
        const size_t r = read_.load(std::memory_order_relaxed);
        read_.store((r + n) % capacityFrames_, std::memory_order_release);
    }

private:
    std::vector<float>  buf_;
    size_t              capacityFrames_ = 0;
    uint16_t            channels_ = 2;
    std::atomic<size_t> write_{ 0 };
    std::atomic<size_t> read_{ 0 };
};
