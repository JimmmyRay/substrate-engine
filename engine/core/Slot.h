#pragma once

#include <utility>

/**
 * @file engine/core/Slot.h
 * @brief A callback whose caller never has to check it.
 *
 * The shape for one function; `engine/Modules.h` is the shape for a subsystem. Where the
 * engine has to reach something it may not name -- see the layering the module directories
 * declare -- an object goes behind an interface there and a callback comes here.
 */
namespace core {

template <typename Signature>
class Slot;

/**
 * @brief A function pointer and a context pointer, defaulting to one that does nothing.
 *
 * An unbound slot answers `R()`, which is what lets a call site drop the null check.
 * `R` must therefore be `void` or default-constructible, and neither pointer is owned:
 * whatever `bind` was handed has to outlive every call.
 */
template <typename R, typename... Args>
class Slot<R(Args...)> {
  public:
    using Fn = R (*)(void* context, Args...);

    Slot() = default;
    Slot(Fn fn, void* context) : call(fn), context(context) {}

    /// Bind `Method` on `object`, as `Slot<...>::bind<&Type::method>(&object)`.
    template <auto Method, typename T>
    static Slot bind(T* object) {
        return Slot(
            [](void* ctx, Args... args) -> R {
                return (static_cast<T*>(ctx)->*Method)(std::forward<Args>(args)...);
            },
            object);
    }

    R operator()(Args... args) const { return call(context, std::forward<Args>(args)...); }

  private:
    static R ignore(void*, Args...) { return R(); }

    Fn call = &ignore;
    void* context = nullptr;
};

} // namespace core
