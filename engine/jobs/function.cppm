module;

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

export module ngine.jobs:function;

import ngine.core;

export namespace ngine::jobs {

// Platz für das Funktionsobjekt eines Jobs. Größere Daten übergibt man per Zeiger.
inline constexpr usize job_function_capacity = 80;

// Funktionsobjekt ohne Heap: speichert ein aufrufbares Objekt bis job_function_capacity Bytes
// direkt in sich. Weder kopier- noch verschiebbar, damit Jobs an ihrem Platz bleiben.
// Wirft das Objekt beim Aufruf eine Ausnahme, endet das Programm (std::terminate).
class JobFunction {
public:
    JobFunction() = default;
    ~JobFunction() {
        reset();
    }
    JobFunction(const JobFunction&) = delete;
    JobFunction& operator=(const JobFunction&) = delete;

    template <typename F>
        requires std::is_invocable_r_v<void, std::decay_t<F>&>
    void emplace(F&& function) {
        using Stored = std::decay_t<F>;
        static_assert(sizeof(Stored) <= job_function_capacity,
                      "Job-Funktion zu groß: größere Daten per Zeiger übergeben");
        static_assert(alignof(Stored) <= alignof(std::max_align_t),
                      "Job-Funktion braucht mehr Ausrichtung als vorgesehen");
        reset();
        std::construct_at(reinterpret_cast<Stored*>(storage_), std::forward<F>(function));
        invoke_ = [](std::byte* storage) { (*std::launder(reinterpret_cast<Stored*>(storage)))(); };
        destroy_ = [](std::byte* storage) noexcept {
            std::destroy_at(std::launder(reinterpret_cast<Stored*>(storage)));
        };
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(storage_);
            invoke_ = nullptr;
            destroy_ = nullptr;
        }
    }

    void operator()() noexcept {
        invoke_(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

private:
    alignas(std::max_align_t) std::byte storage_[job_function_capacity];
    void (*invoke_)(std::byte*) = nullptr;
    void (*destroy_)(std::byte*) noexcept = nullptr;
};

} // namespace ngine::jobs
