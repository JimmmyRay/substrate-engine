#pragma once

#include <cstdint>

namespace core {

/**
 * @file engine/core/Handle.h
 * @brief One handle shape, with a distinct type per kind of thing.
 *
 * `Handle<BodyTag>` and `Handle<SoundTag>` are unrelated types, so passing a body where a
 * sound belongs is a compile error rather than a silent read of another subsystem's array.
 * The tag is declared and never defined; nothing constructs one.
 *
 * Validity is `generation != 0`, not `index != kInvalid`, and that is load-bearing: handles
 * get memset, copied out of freshly resized vectors and default-constructed into
 * aggregates, and under an index sentinel every one of those reads as a live handle to slot
 * zero -- the first slot every subsystem hands out.
 *
 * Each subsystem keeps its own free list. There is no registry owning handles and no
 * `Resource` base class; what a slot means is the subsystem's business.
 */
template <typename Tag>
struct Handle {
    /// What `index` holds when nothing was issued. Not a validity test -- testing it
    /// instead of `valid()` reads a zeroed handle as slot 0.
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    uint32_t index = kInvalid;
    /// Bumped by the owning subsystem every time the slot is freed. Generation 0 must stay
    /// reserved for "never issued", or a zeroed struct is a live handle.
    uint32_t generation = 0;

    /// Was this handle ever issued? Says nothing about whether the object is still alive --
    /// only the subsystem's own `valid(handle)` compares generations and catches a stale one.
    [[nodiscard]] bool valid() const { return generation != 0; }

    bool operator==(const Handle& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const Handle& o) const { return !(*this == o); }
};

} // namespace core
