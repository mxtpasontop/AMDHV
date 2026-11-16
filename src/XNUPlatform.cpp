#include <src/XNUPlatform.hpp>
#include <src/CPU.hpp>
#include <src/TrapRecovery.hpp>
#if defined(AMDHV_KERNEL_BUILD)
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#endif
namespace XNUPlatform {
static Services services;
static Runtime::AtomicBool configured=false,powerFailed=false;
static Runtime::Gate control;
bool configure(const Services &value) {
    Runtime::Guard guard(control);
    if(!guard || configured || !value.valid()) return false;
    services=value;configured=true;return true;
}
uint32_t currentCPU() {return configured?services.cpu():128;}
uintptr_t currentThread() {return configured?services.thread():0;}
bool readOwnedKernel(uint64_t address,void *out,size_t bytes) {
    // Ownership/wiring must be held by the calling framework hook or its region
    // lease. This routine cannot manufacture a lifetime from a pmap translation.
    return configured && out && bytes && bytes<=4096 && MemoryAccess::span(address,bytes) &&
        (address>>47)==0x1ffff && services.copyNoFault(address,reinterpret_cast<uintptr_t>(out),bytes)==bytes;
}
bool recoverTrap(XNUSavedState64 *state) {
    return configured && TrapRecovery::registry().handle(currentCPU(),currentThread(),state);
}
FaultMemory::Context faultOperations(bool (*installed)(void *),void *context) {
    FaultMemory::Context ops;
    if(!configured || !services.traps.valid() || !installed) return ops;
    ops.context=context;ops.cpu=[](void *) {return currentCPU();};ops.thread=[](void *) {return currentThread();};
    ops.disableInterrupts=[](void *) {return services.traps.setInterrupts(0)!=0;};
    ops.restoreInterrupts=[](void *,bool previous) {services.traps.setInterrupts(previous?1:0);};
    ops.probeInstalled=installed;return ops;
}
bool currentStack(uintptr_t thread,VcpuLifecycle::Buffer &out) {
    out={};
    if(!configured || !services.traps.valid() || !thread || thread!=currentThread()) return false;
    uint64_t bytes=0;
    if(!readOwnedKernel(reinterpret_cast<uint64_t>(services.traps.stackBytes),&bytes,sizeof(bytes)) ||
       bytes<4096 || bytes>1024*1024 || (bytes&4095)) return false;
    uintptr_t address=services.traps.threadStack(thread);
    if(!address || (address&4095) || (address>>47)!=0x1ffff || !MemoryAccess::span(address,size_t(bytes))) return false;
    out={address,size_t(bytes)};return true;
}
VcpuLifecycle::Operations vcpuOperations() {
    VcpuLifecycle::Operations ops;
#if defined(AMDHV_KERNEL_BUILD)
    if(!configured || !services.buffers.valid()) return ops;
    ops.memory.read=[](void *,uint64_t address,void *out,size_t bytes) {return readOwnedKernel(address,out,bytes);};
    ops.thread=[](void *) {return currentThread();};
    ops.retain=[](void *,uintptr_t descriptor) {
        if(!descriptor || (descriptor&7) || (descriptor>>47)!=0x1ffff) return false;
        // The original framework owns this descriptor until our reference is
        // acquired. A safe cast validates its public IOKit class, not lifetime.
        auto buffer=OSDynamicCast(IOBufferMemoryDescriptor,reinterpret_cast<OSObject *>(descriptor));
        if(!buffer) return false;
        buffer->retain();return true;
    };
    ops.release=[](void *,uintptr_t descriptor) {reinterpret_cast<IOBufferMemoryDescriptor *>(descriptor)->release();};
    ops.inspect=[](void *,uintptr_t descriptor,VcpuLifecycle::Buffer &out) {
        // Reviewed getters on the pinned kernel only inspect wired descriptor
        // fields. No getPhysicalSegment()/pmap lock is used from a VMX trap.
        // Resolve the exact reviewed methods rather than assuming the SDK's
        // vtable indices or IOByteCount width match this installed kernel.
        out={services.buffers.bytes(descriptor),size_t(services.buffers.length(descriptor))};
        return MemoryAccess::span(out.address,out.bytes) && (out.address>>47)==0x1ffff;
    };
#endif
    return ops;
}
static uint32_t cpu(void *) {return currentCPU();}
static bool onCPU(void *,uint32_t target,void (*action)(void *),void *argument) {
    if(!configured || target>=128 || !action) return false;
    struct Work {uint32_t target;void (*action)(void *);void *argument;bool visited;};
    Work work{target,action,argument,false};
    services.rendezvous([](void *p) {
        auto &w=*static_cast<Work *>(p);
        if(currentCPU()==w.target) {w.visited=true;w.action(w.argument);}
    },&work);
    return work.visited;
}
static bool features(void *,SVMCPU::Features &out) {
    out={};uint32_t a=0,b=0,c=0,d=0;
    CPUInfo::getCpuid(0x80000000,0,a,b,c,d);if(a<0x8000000a) return false;
    CPUInfo::getCpuid(0,0,a,b,c,d);if(b!=0x68747541 || d!=0x69746e65 || c!=0x444d4163) return false;
    CPUInfo::getCpuid(0x80000001,0,a,b,c,d);out.svm=(c&(1U<<2))!=0;
    CPUInfo::getCpuid(0x80000008,0,a,b,c,d);out.physicalBits=a&255;
    CPUInfo::getCpuid(0x8000000a,0,a,b,c,d);out.asids=b;out.npt=d&1;out.nrips=d&8;
    return true;
}
SVMCPU::Operations cpuOperations(uint32_t count,uint32_t requiredASIDs) {
    SVMCPU::Operations ops;
    if(!configured) return ops;
    ops.cpus=count;ops.requiredASIDs=requiredASIDs;ops.currentCPU=cpu;ops.onCPU=onCPU;ops.features=features;
    ops.read=[](void *,uint32_t msr,uint64_t &out) {return services.readMSR(msr,&out)==0;};
    ops.write=[](void *,uint32_t msr,uint64_t value) {return services.writeMSR(msr,value)==0;};
    return ops;
}
GuestMemory::Operations memoryOperations() {
    GuestMemory::Operations ops;
    if(!configured || !services.maps.valid()) return ops;
    ops.retainMap=[](void *,uintptr_t map) {services.maps.reference(map);};
    ops.releaseMap=[](void *,uintptr_t map) {services.maps.deallocate(map);};
    ops.wire=[](void *,uintptr_t map,uint64_t address,uint32_t permissions,uint64_t &physical) {
        physical=0;
        if(!map || map>UINT64_MAX-0x46 || (address&4095) || address>=(1ULL<<48) || !permissions || (permissions&~7U)) return 4;
        uint16_t pageShift=0;
        if(!readOwnedKernel(map+0x44,&pageShift,sizeof(pageShift)) || pageShift!=12) return 4;
        uint32_t page=0;
        // Retained guest vm_map, kernel wire: user-task aliases/COW cannot
        // establish ownership of the HPA installed in the guest pmap.
        int result=services.maps.wireAndExtract(map,address,permissions,0,&page);
        if(!result) physical=uint64_t(page)<<12;
        // A successful call with an invalid PA is still a wire. Registry records
        // it and unwires it; returning failure here would leak that ownership.
        return result;
    };
    ops.unwire=[](void *,uintptr_t map,uint64_t first,uint64_t end) {
        if(!map || first>=end || (first&4095) || (end&4095) || end>(1ULL<<48)) return 4;
        // Called before source unmap, paired with successful kernel wires only.
        // XNU's kernel unwire panics on holes; Registry retains exact ranges.
        return services.maps.unwire(map,first,end,0);
    };
    ops.readPhysical=[](void *,uint64_t pa,void *out,size_t bytes) {
        if(!out || !bytes || bytes>4096 || pa>UINT64_MAX-(bytes-1)) return false;
        uint64_t base=*services.maps.physmapBase,limit=*services.maps.physmapMax;
        if(!base || limit<=base || (base>>47)!=0x1ffff || pa>=limit-base || bytes>limit-base-pa) return false;
        uint64_t last=(pa+bytes-1)>>12;
        if(last>UINT32_MAX) return false;
        for(uint64_t page=pa>>12;page<=last;++page) if(!services.maps.validPage(uint32_t(page))) return false;
        return MemoryAccess::span(base+pa,bytes) && services.copyNoFault(base+pa,reinterpret_cast<uintptr_t>(out),bytes)==bytes;
    };
    return ops;
}
EPTSource::Operations sourceOperations() {
    EPTSource::Operations ops;
    if(!configured || !services.maps.valid() || !services.maps.tryPmapShared || !services.maps.unlockPmapShared ||
       !services.maps.preemptionEnabled || !services.maps.atInterruptContext) return ops;
    ops.tryLock=[](void *,uintptr_t pmap) {
        // The XNU try lock may spin on its interlock. An exception handler or
        // disabled-preemption caller could have interrupted that owner.
        return pmap && !(pmap&63) && (pmap>>47)==0x1ffff &&
            services.maps.preemptionEnabled() && !services.maps.atInterruptContext() && services.maps.tryPmapShared(pmap)!=0;
    };
    ops.unlock=[](void *,uintptr_t pmap) {services.maps.unlockPmapShared(pmap);};
    ops.root=[](void *,uintptr_t pmap,uint64_t &root) {
        // pmap begins with its RW lock; pm_eptp is +0x60 on the pinned image.
        return pmap<=UINT64_MAX-0x68 && readOwnedKernel(pmap+0x60,&root,sizeof(root));
    };
    ops.readPhysical=memoryOperations().readPhysical;
    return ops;
}
#if defined(AMDHV_KERNEL_BUILD)
static IONotifier *powerNotifier=nullptr;
static HostLifecycle::Controller *host=nullptr;
static CancelAndDrain drainRuns=nullptr;
static void *drainContext=nullptr;
static IOReturn powerMessage(void *,void *,UInt32 message,IOService *,void *argument,vm_size_t size) {
    if(message!=kIOMessageCanSystemSleep && message!=kIOMessageSystemWillSleep &&
       message!=kIOMessageSystemHasPoweredOn && message!=kIOMessageSystemWillNotSleep) return kIOReturnSuccess;
    // Removal must not clear the callback's controller/context while in use.
    // No wait is allowed here: contention fences entry and vetoes optional sleep.
    Runtime::Guard guard(control);
    if(!guard) {
        powerFailed=true;
        if((message==kIOMessageCanSystemSleep || message==kIOMessageSystemWillSleep) && argument &&
           (!size || size>=sizeof(IOPowerStateChangeNotification))) {
            auto note=static_cast<IOPowerStateChangeNotification *>(argument);
            note->returnValue=message==kIOMessageCanSystemSleep?0:5000000;
            if(message==kIOMessageCanSystemSleep) return vetoSleepWakeNotification(note->powerRef);
        }
        return kIOReturnBusy;
    }
    if(!host) return kIOReturnNotReady;
    if(message==kIOMessageCanSystemSleep || message==kIOMessageSystemWillSleep) {
        if(!argument || (size && size<sizeof(IOPowerStateChangeNotification))) return kIOReturnBadArgument;
        auto note=static_cast<IOPowerStateChangeNotification *>(argument);
        auto result=host->suspend(); // Closes new run admission even when busy.
        if(result==HostLifecycle::Result::Busy && drainRuns && drainRuns(drainContext)) result=host->suspend();
        if(result!=HostLifecycle::Result::Success) {
            powerFailed=true;
            if(message==kIOMessageCanSystemSleep) {
                note->returnValue=0;
                return vetoSleepWakeNotification(note->powerRef);
            }
            // Forced sleep cannot be vetoed here. Withhold acknowledgement and
            // fence subsequent entry; bounded cancellation is an acceptance
            // prerequisite, not something an IOReturn can substitute for.
            note->returnValue=5000000;
            return kIOReturnBusy;
        }
        // Synchronous completion: IOServicePM acknowledges when we return.
        note->returnValue=0;
    }
    if(message==kIOMessageSystemHasPoweredOn || message==kIOMessageSystemWillNotSleep) {
        if(host->resume()!=HostLifecycle::Result::Success) {powerFailed=true;return kIOReturnError;}
    }
    return kIOReturnSuccess;
}
bool installPowerNotifications(HostLifecycle::Controller &controller,CancelAndDrain drain,void *context) {
    Runtime::Guard guard(control);
    if(!guard || powerNotifier || !configured || !drain || powerFailed || !controller.idle()) return false;
    host=&controller;drainRuns=drain;drainContext=context;
    powerNotifier=registerPrioritySleepWakeInterest(powerMessage,nullptr);
    if(!powerNotifier) {host=nullptr;drainRuns=nullptr;drainContext=nullptr;return false;}
    return true;
}
bool removePowerNotifications() {
    Runtime::Guard guard(control);
    if(!guard) return false;
    if(powerNotifier) {
        if(!host || host->detachAdapter()!=HostLifecycle::Result::Success) return false;
        powerNotifier->remove();powerNotifier=nullptr;host=nullptr;drainRuns=nullptr;drainContext=nullptr;
    }
    return true;
}
#else
bool installPowerNotifications(HostLifecycle::Controller &,CancelAndDrain,void *) {return false;}
bool removePowerNotifications() {return true;}
#endif
bool powerTransitionFailed() {return powerFailed;}
}
