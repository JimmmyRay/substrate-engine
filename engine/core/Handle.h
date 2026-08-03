#pragma once

#include <cstdint>

namespace core {

/**
 * @file engine/core/Handle.h
 * @brief One handle shape, with a distinct type per kind of thing (C1).
 *
 * Five subsystems used to hand out a bare `uint32_t` and two of them said in their own
 * headers that the index was permanent. `InstanceTable` was the only one with a lifetime
 * model: it pairs a stable slot with a generation counter, so a handle to a destroyed
 * object reports staleness instead of aliasing whatever took the slot. The other five took
 * the slot stability without the pair, which is why nothing but an instance could be
 * destroyed.
 *
 * **This is the one place the C arc introduces a shared type rather than repeating a
 * pattern**, and the justification is the Rule of Threes met twice over: six subsystems
 * need exactly this, they span `core/`, `scene/` and `gfx/`, and it carries no state. The
 * scope table in CLAUDE.md selects a global type for precisely that shape.
 *
 * It is a 16-byte value with two fields and three methods. There is no `Resource` base
 * class, no `virtual void release()`, and no registry that owns handles -- each subsystem
 * keeps its own free list, because what a slot means is that subsystem's business.
 *
 * ### Why the tag
 *
 * `Handle<BodyTag>` and `Handle<SoundTag>` are unrelated types, so passing a body where a
 * sound belongs is a compile error rather than a silent read of a different subsystem's
 * array. It is the same argument G2 makes for `Setting<T>` wrapping a `uint16_t`: the
 * numbers were always distinct, and nothing but a type could say so.
 *
 * The tag is never defined -- only declared -- because nothing ever constructs one.
 *
 * ### Why `generation != 0` and not `index != kInvalid`
 *
 * A zeroed struct has to be invalid. Handles get memset, copied out of freshly resized
 * vectors and default-constructed into aggregates, and under an index sentinel every one
 * of those reads as a live handle to slot zero -- the first slot every subsystem hands
 * out. Reserving generation 0 as "never issued" makes zeroed memory invalid by
 * construction, which is the failure mode worth designing against.
 *
 * `index` still defaults to `kInvalid` so that code comparing the raw index sees something
 * obviously wrong rather than slot 0.
 *
 * ### `valid()` against a subsystem's own validity test
 *
 * Two different questions, and both are worth having:
 *
 * - `handle.valid()` -- was this handle ever issued? Cheap, local, no subsystem needed.
 * - `subsystem.valid(handle)` -- and does it still name a live object? Checks the
 *   generation against the slot's, which is what makes a stale handle detectable.
 *
 * A destroyed object's handle is `valid()` and not `subsystem.valid()`. That is the point:
 * the first says the caller is holding a handle rather than a hole, the second says
 * whether the thing behind it is still there.
 */
template <typename Tag>
struct Handle {
    /// What `index` holds when nothing was issued. Not a second validity test -- see
    /// `valid()`, which reads the generation.
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    uint32_t index = kInvalid;
    /// Bumped by the owning subsystem every time the slot is freed. Zero means this
    /// handle was never issued, which is what makes a zeroed struct invalid.
    uint32_t generation = 0;

    /// Was this handle ever issued? Says nothing about whether the object is still alive
    /// -- ask the subsystem that made it.
    [[nodiscard]] bool valid() const { return generation != 0; }

    bool operator==(const Handle& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const Handle& o) const { return !(*this == o); }
};

} // namespace core
