#ifndef AMDHV_TRAP_ADAPTER_HPP
#define AMDHV_TRAP_ADAPTER_HPP
#include <src/FaultMemory.hpp>
#include <src/VcpuLifecycle.hpp>
namespace TrapAdapter {
// A nonblocking, retained image ticket. The provider must verify image UUIDs,
// executable permissions and instruction-site policy before handing out code.
// It owns every image used by readOnly() until unpinCode(), including unload.
struct Code {uint64_t address {};size_t bytes {};uintptr_t token {};};
struct Operations {
    void *context {};
    FaultMemory::Context faults;
    VcpuLifecycle::Registry *vcpus {};
    bool (*pinCode)(void *,uint64_t pc,Code &) {};
    void (*unpinCode)(void *,const Code &) {};
    bool (*readOnly)(void *,const Code &,uint64_t,size_t) {};
    bool (*stack)(void *,uintptr_t thread,VcpuLifecycle::Buffer &) {};
    bool (*readyCPU)(void *,uint32_t) {};
    // No callback may sleep, allocate, take a blocking lock, or reenter handle.
    // The owner must keep this adapter and its providers alive after configure.
};
class Adapter {
    Operations ops_;
    Runtime::Gate control_;
    Runtime::AtomicBool configured_;
    uint8_t active_[128] {};
public:
    bool configure(Operations operations);
    bool ready() const {return configured_;}
    bool handle(XNUSavedState64 *state);
};
Adapter &global();
}
#endif
