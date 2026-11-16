#ifndef AMDHV_XNU_PLATFORM_HPP
#define AMDHV_XNU_PLATFORM_HPP
#include <src/SVMCPU.hpp>
#include <src/MemoryAccess.hpp>
#include <src/HostLifecycle.hpp>
#include <src/GuestMemory.hpp>
#include <src/EPTSource.hpp>
#include <src/VcpuLifecycle.hpp>
#include <src/FaultMemory.hpp>
namespace XNUPlatform {
struct BufferServices {
    uintptr_t (*bytes)(uintptr_t descriptor) {};
    uint64_t (*length)(uintptr_t descriptor) {};
    bool valid() const {return bytes && length;}
};
struct TrapServices {
    int (*setInterrupts)(int enabled) {};
    uintptr_t (*threadStack)(uintptr_t thread) {};
    const uint64_t *stackBytes {};
    bool valid() const {return setInterrupts && threadStack && stackBytes;}
};
struct MapServices {
    void (*reference)(uintptr_t) {};
    void (*deallocate)(uintptr_t) {};
    int (*wireAndExtract)(uintptr_t,uint64_t,uint32_t,int,uint32_t *) {};
    int (*unwire)(uintptr_t,uint64_t,uint64_t,int) {};
    int (*validPage)(uint32_t) {};
    int (*tryPmapShared)(uintptr_t) {};
    void (*unlockPmapShared)(uintptr_t) {};
    int (*preemptionEnabled)() {};
    int (*atInterruptContext)() {};
    const uint64_t *physmapBase {},*physmapMax {};
    bool valid() const {return reference && deallocate && wireAndExtract && unwire && validPage && physmapBase && physmapMax;}
};
struct Services {
    uint32_t (*cpu)() {};
    uintptr_t (*thread)() {};
    void (*rendezvous)(void (*)(void *),void *) {};
    int (*readMSR)(uint32_t,uint64_t *) {};
    int (*writeMSR)(uint32_t,uint64_t) {};
    size_t (*copyNoFault)(uintptr_t,uintptr_t,size_t) {};
    MapServices maps;
    BufferServices buffers;
    TrapServices traps;
    bool valid() const {return cpu && thread && rendezvous && readMSR && writeMSR && copyNoFault;}
};
// Installation copies function pointers only, with no CPU/MSR or IOKit effects.
bool configure(const Services &services);
SVMCPU::Operations cpuOperations(uint32_t cpus,uint32_t requiredASIDs);
GuestMemory::Operations memoryOperations();
EPTSource::Operations sourceOperations();
VcpuLifecycle::Operations vcpuOperations();
FaultMemory::Context faultOperations(bool (*probeInstalled)(void *),void *context=nullptr);
bool currentStack(uintptr_t thread,VcpuLifecycle::Buffer &out);
bool readOwnedKernel(uint64_t address,void *out,size_t bytes);
uintptr_t currentThread();
uint32_t currentCPU();
bool recoverTrap(XNUSavedState64 *state);
// Called only in the sleepable IOService setup/teardown path.
using CancelAndDrain = bool (*)(void *);
// CancelAndDrain must provide bounded vCPU cancellation; no notification is
// installed without it. Returning true is followed by a run-count recheck.
// Install on an idle configured controller before advertising acquisition.
// Controller and drain context outlive registration. Drain must not reenter
// XNUPlatform notification control. Removal refuses outstanding ownership/runs.
// Successful removal atomically detaches the controller's CPU adapter.
bool installPowerNotifications(HostLifecycle::Controller &controller,CancelAndDrain drain,void *context);
bool removePowerNotifications();
bool powerTransitionFailed();
}
#endif
