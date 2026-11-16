#ifndef AMDHV_RUNTIME_HPP
#define AMDHV_RUNTIME_HPP
#include <stdint.h>
// Nonblocking admission: trap/reentrant callers fail rather than wait on an
// interrupted owner. No allocation, interrupt masking, or scheduler dependency.
namespace Runtime {
class AtomicBool {
    uint8_t value_;
public:
    constexpr AtomicBool(bool value=false):value_(value) {}
    AtomicBool(const AtomicBool &other):value_(bool(other)) {}
    operator bool() const {return __atomic_load_n(&value_,__ATOMIC_ACQUIRE)!=0;}
    AtomicBool &operator=(bool value) {__atomic_store_n(&value_,uint8_t(value),__ATOMIC_RELEASE);return *this;}
    AtomicBool &operator=(const AtomicBool &other) {return *this=bool(other);}
};
class Gate {
    uint8_t held_ {0};
public:
    Gate()=default;
    Gate(const Gate &)=delete;
    Gate &operator=(const Gate &)=delete;
    bool enter() { return !__atomic_test_and_set(&held_, __ATOMIC_ACQUIRE); }
    void leave() { __atomic_clear(&held_, __ATOMIC_RELEASE); }
};
class Guard {
    Gate *gate_;
public:
    explicit Guard(Gate &gate) : gate_(gate.enter() ? &gate : nullptr) {}
    ~Guard() { if (gate_) gate_->leave(); }
    explicit operator bool() const { return gate_ != nullptr; }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
};
}
#endif
