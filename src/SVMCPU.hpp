#ifndef AMDHV_SVM_CPU_HPP
#define AMDHV_SVM_CPU_HPP
#include <src/AMDSVM.hpp>
#include <src/IntelVMX.hpp>
#include <src/HostLifecycle.hpp>
#include <src/Runtime.hpp>
namespace SVMCPU {
struct Features {uint32_t asids {};uint8_t physicalBits {};bool svm {},npt {},nrips {};};
struct Operations {
    void *context {};
    uint32_t cpus {};
    uint32_t requiredASIDs {2};
    uint32_t (*currentCPU)(void *) {};
    bool (*onCPU)(void *,uint32_t,void (*)(void *),void *) {};
    bool (*features)(void *,Features &) {};
    bool (*read)(void *,uint32_t,uint64_t &) {};
    bool (*write)(void *,uint32_t,uint64_t) {};
};
// Control-plane object, never allocated or destroyed by a rendezvous callback.
// The owner must quiesce guest runs before restore/suspend and outlive its adapter.
class Driver {
    struct Saved {uint64_t efer {},hsave {},ours {};bool dirty {};Runtime::AtomicBool enabled;};
    Operations ops_;
    Saved saved_[128] {};
    Runtime::Gate gate_;
    bool configured_ {};
    struct Job {Driver *driver;uint32_t cpu;bool enable,visited,ok;};
    static void dispatch(void *argument);
    bool enableLocal(uint32_t cpu);
    bool restoreLocal(uint32_t cpu);
public:
    bool configure(Operations operations,const uint64_t *hostSavePages,size_t count);
    bool enable(uint32_t cpu);
    bool restore(uint32_t cpu);
    bool ownsCPU(uint32_t cpu) const;
    // Readiness is published only after both MSRs were read back. Ownership
    // includes partially enabled CPUs retained for rollback and is insufficient
    // to admit a VMX trap or guest transition.
    bool readyCPU(uint32_t cpu) const;
    HostLifecycle::Adapter adapter();
};
}
#endif
