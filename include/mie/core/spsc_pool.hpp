#pragma once
#include <cassert>
#include <cstddef>
#include <memory>
#include "mie/core/spsc_ring.hpp"

namespace mie {

// Fixed-size pool for exactly ONE allocating thread and ONE freeing thread.
// Free slots travel back to the allocator through an SPSC ring, so there is no
// shared free-list head and no CAS. Usable slots = RingCapacity - 1
// (the ring keeps one cell empty). The object is large: heap-allocate it.
template <typename T, size_t RingCapacity>
class SpscPool {
    using FreeRing = SpscRing<T*, RingCapacity>;

public:
    static constexpr size_t kSlots = RingCapacity - 1;

    // new T[n]() zeroes every slot, which touches all the pages now, so page
    // faults don't land inside a timed run (the old pool also touched every slot).
    SpscPool() : storage_(new T[kSlots]()) {
        for (size_t i = 0; i < kSlots; ++i) free_.push(&storage_[i]);
    }

    // Call from the allocating thread only. Returns nullptr when every slot is in use.
    T* allocate() {
        auto p = free_.pop();
        return p ? *p : nullptr;
    }

    // Call from the freeing thread only.
    void deallocate(T* p) {
        [[maybe_unused]] const bool ok = free_.push(p);
        assert(ok && "free ring cannot be full unless a slot was freed twice");
    }

private:
    std::unique_ptr<T[]> storage_;
    FreeRing free_;
};

} // namespace mie