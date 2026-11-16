#ifndef AMDHV_HOST_LIFECYCLE_HPP
#define AMDHV_HOST_LIFECYCLE_HPP
#include <src/Runtime.hpp>
#include <stdint.h>
namespace HostLifecycle {
enum class Result { Success, Busy, Conflict, Unavailable, HardwareFailure };
enum class State { Unconfigured, Idle, Ready, Quiescing, Suspended, Faulted };
struct Adapter {
    void *context {};
    uint32_t cpus {};
    // Sleepable control context. A kernel implementation must rendezvous with
    // each CPU and preserve/restore its original EFER and VM_HSAVE_PA values.
    bool (*enable)(void *,uint32_t) {};
    bool (*restore)(void *,uint32_t) {};
};
class Controller {
    Runtime::Gate gate_;
    Adapter adapter_;
    State state_ {State::Unconfigured};
    uint32_t users_ {0}, runs_ {0};
    bool exclusive_ {false}, enabled_[128] {};
    uint8_t running_[128] {};
    bool restoreAll() {
        bool ok=true;
        for(uint32_t i=0;i<adapter_.cpus;++i) if(enabled_[i]) {
            if(adapter_.restore(adapter_.context,i)) enabled_[i]=false;
            else ok=false;
        }
        return ok;
    }
    bool enableAll() {
        for(uint32_t i=0;i<adapter_.cpus;++i) {
            // A failed enable may have changed hardware before reporting failure.
            // Include that CPU in rollback; restore must be idempotent for an
            // untouched CPU and retain ownership when restoration itself fails.
            enabled_[i]=true;
            if(!adapter_.enable(adapter_.context,i)) {
                state_=restoreAll()?State::Idle:State::Faulted;
                return false;
            }
        }
        state_=State::Ready;return true;
    }
    bool detachable() const {
        if(users_ || __atomic_load_n(&runs_,__ATOMIC_ACQUIRE) ||
           (state_!=State::Unconfigured && state_!=State::Idle && state_!=State::Suspended)) return false;
        for(bool enabled:enabled_) if(enabled) return false;
        return true;
    }
public:
    bool canDetachAdapter() {
        Runtime::Guard guard(gate_);return guard && detachable();
    }
    bool idle() {
        Runtime::Guard guard(gate_);return guard && state_==State::Idle && detachable();
    }
    Result detachAdapter() {
        Runtime::Guard guard(gate_);if(!guard) return Result::Busy;
        if(!detachable()) return Result::Conflict;
        // Fence new acquisitions atomically with the ownership check. A query
        // followed by notifier removal would leave an acquisition race.
        adapter_={};state_=State::Unconfigured;return Result::Success;
    }
    Result configure(Adapter adapter) {
        Runtime::Guard guard(gate_);
        if(!guard) return Result::Busy;
        if(state_!=State::Unconfigured && state_!=State::Idle) return Result::Conflict;
        if(!adapter.cpus || adapter.cpus>128 || !adapter.enable || !adapter.restore) return Result::Unavailable;
        adapter_=adapter;state_=State::Idle;return Result::Success;
    }
    Result acquire(bool exclusive) {
        Runtime::Guard guard(gate_);
        if(!guard) return Result::Busy;
        if(state_==State::Unconfigured || state_==State::Suspended || state_==State::Quiescing || state_==State::Faulted) return Result::Unavailable;
        if(exclusive_ || (exclusive && users_) || users_==UINT32_MAX) return Result::Conflict;
        if(!users_ && !enableAll()) return Result::HardwareFailure;
        ++users_;exclusive_=exclusive;return Result::Success;
    }
    Result release() {
        Runtime::Guard guard(gate_);
        if(!guard || __atomic_load_n(&runs_,__ATOMIC_ACQUIRE)) return Result::Busy;
        if(!users_) return Result::Conflict;
        if(users_>1) {--users_;return Result::Success;}
        if(!restoreAll()) {state_=State::Faulted;return Result::HardwareFailure;}
        users_=0;exclusive_=false;
        // Closing the last VM while asleep/quiescing must not reopen admission.
        if(state_!=State::Suspended && state_!=State::Quiescing) state_=State::Idle;
        return Result::Success;
    }
    class Run {
        Controller *host_ {};uint32_t cpu_ {};
        Run(Controller *host,uint32_t cpu):host_(host),cpu_(cpu) {}
        friend class Controller;
    public:
        Run()=default;
        ~Run() {reset();}
        Run(const Run &)=delete;Run &operator=(const Run &)=delete;
        Run(Run &&other) noexcept:host_(other.host_),cpu_(other.cpu_) {other.host_=nullptr;}
        void reset() {
            if(!host_) return;
            __atomic_clear(&host_->running_[cpu_],__ATOMIC_RELEASE);
            __atomic_sub_fetch(&host_->runs_,1U,__ATOMIC_RELEASE);
            host_=nullptr;
        }
        explicit operator bool() const {return host_!=nullptr;}
    };
    Run beginRun(uint32_t cpu) {
        Runtime::Guard guard(gate_);
        if(!guard || state_!=State::Ready || !users_ || cpu>=adapter_.cpus || !enabled_[cpu] ||
            __atomic_load_n(&runs_,__ATOMIC_RELAXED)==UINT32_MAX ||
            __atomic_test_and_set(&running_[cpu],__ATOMIC_ACQUIRE)) return {};
        __atomic_add_fetch(&runs_,1U,__ATOMIC_RELAXED);
        return Run(this,cpu);
    }
    Result suspend() {
        Runtime::Guard guard(gate_);
        if(!guard) return Result::Busy;
        if(state_==State::Suspended) return Result::Success;
        if(state_!=State::Ready && state_!=State::Idle && state_!=State::Quiescing) return Result::Unavailable;
        state_=State::Quiescing; // Close admission before waiting for existing runs.
        if(__atomic_load_n(&runs_,__ATOMIC_ACQUIRE)) return Result::Busy;
        if(!restoreAll()) {state_=State::Faulted;return Result::HardwareFailure;}
        state_=State::Suspended;return Result::Success;
    }
    Result recover() {
        Runtime::Guard guard(gate_);
        if(!guard || __atomic_load_n(&runs_,__ATOMIC_ACQUIRE)) return Result::Busy;
        if(state_!=State::Faulted || users_) return Result::Conflict;
        if(!restoreAll()) return Result::HardwareFailure;
        state_=State::Idle;return Result::Success;
    }
    Result resume() {
        Runtime::Guard guard(gate_);
        if(!guard) return Result::Busy;
        if(state_==State::Ready || state_==State::Idle) return Result::Success;
        if(state_==State::Quiescing) {state_=users_?State::Ready:State::Idle;return Result::Success;}
        if(state_!=State::Suspended) return Result::Conflict;
        if(users_ && !enableAll()) {state_=State::Faulted;return Result::HardwareFailure;}
        state_=users_?State::Ready:State::Idle;return Result::Success;
    }
};
}
#endif
