#ifndef AMDHV_BACKEND_POLICY_HPP
#define AMDHV_BACKEND_POLICY_HPP

// These are acceptance gates, not user-overridable build flags. Completing a
// gate requires the corresponding tests and hardware evidence in docs/IMPLEMENTATION.md.
namespace BackendPolicy {
enum class Mode : int { Bootstrap = 0, Preflight = 1, Full = 2, Trap = 3 };
inline bool sameName(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
inline bool parseMode(const char *name, Mode &mode) {
    if (sameName(name, "bootstrap")) mode = Mode::Bootstrap;
    else if (sameName(name, "preflight")) mode = Mode::Preflight;
    else if (sameName(name, "trap")) mode = Mode::Trap;
    else if (sameName(name, "full")) mode = Mode::Full;
    else return false;
    return true;
}
constexpr bool compatibilityVerified = false;
constexpr bool synchronizedOwnership = false;
constexpr bool synchronousShootdowns = false;
constexpr bool faultSafeTransition = false;
constexpr bool architecturalControls = false;
constexpr bool executionEnabled = compatibilityVerified && synchronizedOwnership &&
    synchronousShootdowns && faultSafeTransition && architecturalControls;
}
#endif
