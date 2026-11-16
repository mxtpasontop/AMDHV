#include <src/AMDHV.hpp>
#include <src/BackendPolicy.hpp>
#include <src/Compatibility.hpp>
#include <src/HostLifecycle.hpp>
#include <src/Runtime.hpp>
#include <BuildInfo.hpp>
#include <ABIInfo.hpp>
#include <src/SoftVMCS.hpp>
#include <src/NPTManager.hpp>
#include <src/VMCBTranslator.hpp>
#include <src/CPU.hpp>
#include <src/XNUPlatform.hpp>
#include <src/AppleHVABI.hpp>
#include <src/SpaceLifecycle.hpp>
#include <src/VcpuLifecycle.hpp>
#include <src/TrapAdapter.hpp>

#if defined(AMDHV_KERNEL_BUILD)
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
extern "C" {
    uint32_t cpu_number(void);
    void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);
}
#endif

bool amdhvDebugEnabled = false;
#if defined(AMDHV_KERNEL_BUILD)
uint32_t ADDPR(debugPrintDelay) = 0;
#endif

static AMDHVPlugin gAMDHVPlugin;

static const char *kAppleHVPaths[] = {
    "/System/Library/Extensions/AppleHV.kext/Contents/MacOS/AppleHV"
};

static KernelPatcher::KextInfo kAppleHVKextInfo = {
    "com.apple.driver.AppleHV",
    kAppleHVPaths,
    arrsize(kAppleHVPaths),
    {true, false, false, false, false, false},
    {false, false},
    KernelPatcher::KextInfo::Unloaded
};

// Original function trampolines
static mach_vm_address_t orgHvSupportInit           = 0;
static mach_vm_address_t orgHvGetSupport            = 0;
static mach_vm_address_t orgHostVmxon               = 0;
static mach_vm_address_t orgHostVmxoff              = 0;
static mach_vm_address_t orgKernelTrap              = 0;
static mach_vm_address_t orgVcpuResumeOrLaunchGuest = 0;
static mach_vm_address_t orgVcpuInitVMCS            = 0;
static mach_vm_address_t orgVcpuFree                = 0;
static mach_vm_address_t orgSpaceEptFault           = 0;
static mach_vm_address_t orgSpaceInit               = 0;
static mach_vm_address_t orgSpaceMap                = 0;
static mach_vm_address_t orgSpaceMapKernel          = 0;
static mach_vm_address_t orgSpaceProtect            = 0;
static mach_vm_address_t orgSpaceUnmap              = 0;
static mach_vm_address_t orgSpaceInvalidate         = 0;
static mach_vm_address_t orgSpaceFree               = 0;

static int *gVmxHvSupportPtr = nullptr;
static Runtime::AtomicBool gXnuReady = false;
static Runtime::AtomicBool gAppleHVReady = false;
static Runtime::AtomicBool gHookFailure = false;
static Runtime::AtomicBool gRegistrationAttempted = false;
static Runtime::AtomicBool gCallbacksRegistered = false;
// 0: bootstrap/service only; 1: Lilu callbacks + symbol checks, no writes;
// 2: experimental translation hooks; 3: trap trampoline only, no emulation.
// Full activation must be explicit.
static int gDiagnosticStage = 0;
static Runtime::AtomicBool gTrapHookReady = false;
static Runtime::AtomicBool gXnuPreflightComplete=false,gApplePreflightComplete=false;
// Early callbacks run before IOKit is ready. Only retain state here; the
// IOService publishes it later in start(). Never allocate registry properties
// from a Lilu patcher callback.
static const char *gState = "bootstrap";
static char gDetail[128] = {};
static Runtime::Gate gStateGate, gInitGate;
static Runtime::AtomicBool gStateLost=false;
// CPU/MSR and power bindings exist, but hardware/lifecycle acceptance has not
// configured this controller. Acquisition fails until that setup is accepted.
static HostLifecycle::Controller gHostLifecycle;

bool AMDHVPlugin::detectAMDSVM(uint32_t &outMaxASID, bool &outHasNP, bool &outHasNRIPS) {
    outMaxASID=0;outHasNP=outHasNRIPS=false;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    CPUInfo::getCpuid(0x80000000U,0,eax,ebx,ecx,edx);
    if(eax<0x8000000aU) return false;
    CPUInfo::getCpuid(0,0,eax,ebx,ecx,edx);
    if(ebx!=0x68747541 || edx!=0x69746e65 || ecx!=0x444d4163) return false;
    CPUInfo::getCpuid(0x80000001U, 0, eax, ebx, ecx, edx);
    // ECX Bit 2 = SVM
    if ((ecx & (1U << 2)) == 0) {
        return false;
    }

    CPUInfo::getCpuid(0x8000000AU, 0, eax, ebx, ecx, edx);
    outMaxASID  = ebx;
    outHasNP    = (edx & (1U << 0)) != 0; // Bit 0 = Nested Paging
    outHasNRIPS = (edx & (1U << 3)) != 0; // Bit 3 = Next RIP Save
    return outHasNP && outHasNRIPS && outMaxASID>=2;
}

static void AMDHVPublishState(const char *state,const char *detail) {
    Runtime::Guard guard(gStateGate);
    if(!guard) {gStateLost=true;return;}
    gState=state;
    if(detail!=gDetail) snprintf(gDetail,sizeof(gDetail),"%s",detail?detail:"");
    SYSLOG("AMDHV","build=%s state=%s detail=%s",AMDHV_BUILD_ID,gState,gDetail);
}

static void publishPreflight() {
    if(gHookFailure) return;
    if(gXnuPreflightComplete && gApplePreflightComplete)
        AMDHVPublishState("diagnostic-applehv-preflight","kernel/AppleHV UUIDs and symbols checked; no hooks installed");
    else if(gXnuPreflightComplete)
        AMDHVPublishState("diagnostic-xnu-preflight","XNU checked; waiting for AppleHV preflight");
    else AMDHVPublishState("diagnostic-waiting-xnu","AppleHV checked; waiting for XNU preflight");
}

static bool hooksReady() {
    return BackendPolicy::executionEnabled && gDiagnosticStage == 2 && gXnuReady && gAppleHVReady && !gHookFailure &&
        !XNUPlatform::powerTransitionFailed() && gTrapHookReady && SpaceLifecycle::global().ready() &&
        VcpuLifecycle::global().ready() && TrapAdapter::global().ready();
}

static void updateHvSupport() {
    if (gDiagnosticStage != 2) return;
    if (gVmxHvSupportPtr) __atomic_store_n(gVmxHvSupportPtr,hooksReady()?1:0,__ATOMIC_RELEASE);
}

static bool preflightRoutes(KernelPatcher &patcher, size_t id,
                            KernelPatcher::RouteRequest *requests, size_t count) {
    // Explicit diagnostic preflight. Lilu also resolves all symbols before
    // routing, but its subsequent patch operations are not transactional.
    for (size_t i = 0; i < count; ++i) {
        if (!patcher.solveSymbol(id, requests[i].symbol)) {
            gHookFailure = true;
            updateHvSupport();
            AMDHVPublishState("unsupported-symbols", requests[i].symbol);
            patcher.clearError();
            return false;
        }
    }
    return true;
}

static bool verifyInstructionContracts(KernelPatcher &patcher,size_t id,bool kernel) {
#if defined(AMDHV_KERNEL_BUILD)
    for(const auto &item:ABIProfile::evidence) {
        if(item.kernel!=kernel) continue;
        auto address=patcher.solveSymbol(id,item.symbol);
        if(!address || item.offset>UINT64_MAX-address ||
           memcmp(reinterpret_cast<const void *>(address+item.offset),item.bytes,item.size)) {
            gHookFailure=true;patcher.clearError();AMDHVPublishState("unsupported-abi",item.symbol);return false;
        }
    }
#else
    (void)patcher;(void)id;(void)kernel;
#endif
    return true;
}

static bool routeWithDiagnostics(KernelPatcher &patcher, size_t id,
        KernelPatcher::RouteRequest *requests, size_t count,
        mach_vm_address_t slide = 0, size_t size = 0) {
    for (size_t i = 0; i < count; ++i) {
        SYSLOG("AMDHV", "route-begin %s", requests[i].symbol);
        if (!patcher.routeMultiple(id, &requests[i], 1, slide, size)) {
            SYSLOG("AMDHV", "route-failed %s", requests[i].symbol);
            return false;
        }
        SYSLOG("AMDHV", "route-complete %s", requests[i].symbol);
        if(requests[i].org==&orgKernelTrap && orgKernelTrap) gTrapHookReady=true;
    }
    return true;
}

bool AMDHVPlugin::init() {
    Runtime::Guard guard(gInitGate);
    if(!guard) return false;
    // The audit found missing isolation and hardware recovery contracts.
    // Symbol availability alone must never authorize executable-code writes.
    if (gDiagnosticStage != 0 && gDiagnosticStage != 1) {
        gHookFailure = true;
        AMDHVPublishState("blocked-backend", "translation/trap routing require verified compatibility and isolation");
        return false;
    }
    if (gDiagnosticStage == 0) {
        AMDHVPublishState("diagnostic-bootstrap", "no Lilu callbacks or hooks");
        return true;
    }
    if (gRegistrationAttempted) return gCallbacksRegistered;
    gRegistrationAttempted = true;
    AMDHVPublishState("waiting-lilu-patcher","registering callbacks");
    auto err = lilu.onKextLoad(&kAppleHVKextInfo, 1, &AMDHVPlugin::onKextLoad, this);
    if (err == LiluAPI::Error::NoError)
        err = lilu.onPatcherLoad(&AMDHVPlugin::onPatcherLoad, this);
    if (err != LiluAPI::Error::NoError) {
        gHookFailure = true;
        char detail[64];
        snprintf(detail, sizeof(detail), "Lilu callback registration: %d", static_cast<int>(err));
        AMDHVPublishState("registration-failed", detail);
        return false;
    }
    gCallbacksRegistered = true;
    return true;
}

void AMDHVPlugin::deinit() {
}

void AMDHVPlugin::onPatcherLoad(void *user, KernelPatcher &patcher) {
    (void)user;
    if (gHookFailure || (!BackendPolicy::executionEnabled && gDiagnosticStage!=1)) return;
    AMDHVPublishState("patcher-loaded", "checking XNU compatibility");
    uint32_t maxASID = 0;
    bool hasNP = false, hasNRIPS = false;
    if (!detectAMDSVM(maxASID, hasNP, hasNRIPS) || maxASID < 2) {
        gHookFailure = true;
        AMDHVPublishState("unsupported-cpu", "SVM, NPT and guest ASIDs required");
        return;
    }
    auto uuidAddress = patcher.solveSymbol(KernelPatcher::KernelID, "_kernel_uuid");
    if (!uuidAddress || !Compatibility::equalUUID(reinterpret_cast<const unsigned char *>(uuidAddress), Compatibility::kernelUUID)) {
        gHookFailure = true;
        patcher.clearError();
        AMDHVPublishState("unsupported-build", "kernel UUID mismatch");
        return;
    }
    if(!verifyInstructionContracts(patcher,KernelPatcher::KernelID,true)) return;
#if defined(AMDHV_KERNEL_BUILD)
    XNUPlatform::Services services;
    services.cpu=reinterpret_cast<decltype(services.cpu)>(patcher.solveSymbol(KernelPatcher::KernelID,"_cpu_number"));
    services.thread=reinterpret_cast<decltype(services.thread)>(patcher.solveSymbol(KernelPatcher::KernelID,"_current_thread"));
    services.rendezvous=reinterpret_cast<decltype(services.rendezvous)>(patcher.solveSymbol(KernelPatcher::KernelID,"_mp_rendezvous_no_intrs"));
    services.readMSR=reinterpret_cast<decltype(services.readMSR)>(patcher.solveSymbol(KernelPatcher::KernelID,"_rdmsr64_carefully"));
    services.writeMSR=reinterpret_cast<decltype(services.writeMSR)>(patcher.solveSymbol(KernelPatcher::KernelID,"_wrmsr_carefully"));
    services.copyNoFault=reinterpret_cast<decltype(services.copyNoFault)>(patcher.solveSymbol(KernelPatcher::KernelID,"_ml_nofault_copy"));
    services.maps.reference=reinterpret_cast<decltype(services.maps.reference)>(patcher.solveSymbol(KernelPatcher::KernelID,"_vm_map_reference"));
    services.maps.deallocate=reinterpret_cast<decltype(services.maps.deallocate)>(patcher.solveSymbol(KernelPatcher::KernelID,"_vm_map_deallocate"));
    services.maps.wireAndExtract=reinterpret_cast<decltype(services.maps.wireAndExtract)>(patcher.solveSymbol(KernelPatcher::KernelID,"_vm_map_wire_and_extract"));
    services.maps.unwire=reinterpret_cast<decltype(services.maps.unwire)>(patcher.solveSymbol(KernelPatcher::KernelID,"_vm_map_unwire"));
    services.maps.validPage=reinterpret_cast<decltype(services.maps.validPage)>(patcher.solveSymbol(KernelPatcher::KernelID,"_pmap_valid_page"));
    services.maps.physmapBase=reinterpret_cast<decltype(services.maps.physmapBase)>(patcher.solveSymbol(KernelPatcher::KernelID,"_physmap_base"));
    services.maps.physmapMax=reinterpret_cast<decltype(services.maps.physmapMax)>(patcher.solveSymbol(KernelPatcher::KernelID,"_physmap_max"));
    services.maps.tryPmapShared=reinterpret_cast<decltype(services.maps.tryPmapShared)>(patcher.solveSymbol(KernelPatcher::KernelID,"_lck_rw_try_lock_shared"));
    services.maps.unlockPmapShared=reinterpret_cast<decltype(services.maps.unlockPmapShared)>(patcher.solveSymbol(KernelPatcher::KernelID,"_lck_rw_unlock_shared"));
    services.maps.preemptionEnabled=reinterpret_cast<decltype(services.maps.preemptionEnabled)>(patcher.solveSymbol(KernelPatcher::KernelID,"_preemption_enabled"));
    services.maps.atInterruptContext=reinterpret_cast<decltype(services.maps.atInterruptContext)>(patcher.solveSymbol(KernelPatcher::KernelID,"_ml_at_interrupt_context"));
    if(!services.maps.tryPmapShared || !services.maps.unlockPmapShared || !services.maps.preemptionEnabled || !services.maps.atInterruptContext) {
        gHookFailure=true;patcher.clearError();AMDHVPublishState("unsupported-abi","XNU pmap/context services unavailable");return;
    }
    if(!services.maps.valid()) {gHookFailure=true;patcher.clearError();AMDHVPublishState("unsupported-abi","XNU map services unavailable");return;}
    services.buffers.bytes=reinterpret_cast<decltype(services.buffers.bytes)>(patcher.solveSymbol(KernelPatcher::KernelID,"__ZN24IOBufferMemoryDescriptor14getBytesNoCopyEv"));
    services.buffers.length=reinterpret_cast<decltype(services.buffers.length)>(patcher.solveSymbol(KernelPatcher::KernelID,"__ZNK18IOMemoryDescriptor9getLengthEv"));
    if(!services.buffers.valid()) {gHookFailure=true;patcher.clearError();AMDHVPublishState("unsupported-abi","XNU wired-buffer getters unavailable");return;}
    services.traps.setInterrupts=reinterpret_cast<decltype(services.traps.setInterrupts)>(patcher.solveSymbol(KernelPatcher::KernelID,"_ml_set_interrupts_enabled"));
    services.traps.threadStack=reinterpret_cast<decltype(services.traps.threadStack)>(patcher.solveSymbol(KernelPatcher::KernelID,"_thread_get_kernel_stack"));
    services.traps.stackBytes=reinterpret_cast<decltype(services.traps.stackBytes)>(patcher.solveSymbol(KernelPatcher::KernelID,"_kernel_stack_size"));
    if(!services.traps.valid()) {gHookFailure=true;patcher.clearError();AMDHVPublishState("unsupported-abi","XNU trap ownership services unavailable");return;}
    if(!XNUPlatform::configure(services)) {gHookFailure=true;AMDHVPublishState("unsupported-abi","XNU services unavailable or already configured");return;}
#endif
    SoftVMCS::init();
    NPTManager::init();
    SYSLOG("AMDHV", "Checking XNU UUID, CPU capabilities and required symbols...");

    // Resolve XNU's physical memory map base (_physmap_base) so NPTManager can walk EPT tables
    mach_vm_address_t physmapSym = patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base");
    if (physmapSym) {
        // Presence only: the owned physical-access adapter replaces unchecked
        // physmap dereferences. Preflight never reads source page tables.
    } else {
        gHookFailure = true;
        patcher.clearError();
        AMDHVPublishState("unsupported-symbols", "_physmap_base");
        return;
    }

    // Keep the support global off until both hook sets are ready.
    mach_vm_address_t hvSupportSym = patcher.solveSymbol(KernelPatcher::KernelID, "_hv_support_available");
    if (hvSupportSym) {
        gVmxHvSupportPtr = reinterpret_cast<int *>(hvSupportSym);
        // Reading the symbol is allowed in preflight mode; do not write it.
    } else {
        gHookFailure = true;
        patcher.clearError();
        AMDHVPublishState("unsupported-symbols", "_hv_support_available");
        return;
    }

    KernelPatcher::RouteRequest kernelRequests[] = {
        // Trap handling must precede every capability override: pmap and
        // AppleHV may execute Intel-only MSRs as soon as support becomes true.
        {"_kernel_trap",     hookedKernelTrap,    orgKernelTrap},
        {"_vmx_hv_support",  hookedHvGetSupport},
        {"_hv_support_init", hookedHvSupportInit, orgHvSupportInit},
        {"_hv_get_support",  hookedHvGetSupport,  orgHvGetSupport},
        {"_host_vmxon",      hookedHostVmxon,     orgHostVmxon},
        {"_host_vmxoff",     hookedHostVmxoff,    orgHostVmxoff},
    };

    if (!preflightRoutes(patcher, KernelPatcher::KernelID, kernelRequests, arrsize(kernelRequests))) return;
    if (gDiagnosticStage == 1) {
        gXnuPreflightComplete=true;publishPreflight();
        return;
    }
    if (gDiagnosticStage == 3) {
        // Exercise only the trap detour/trampoline. No capability overrides,
        // SVM setup, VMX emulation, or AppleHV hooks participate in this test.
        if (!routeWithDiagnostics(patcher, KernelPatcher::KernelID, kernelRequests, 1)) {
            gHookFailure = true;
            patcher.clearError();
            AMDHVPublishState("trap-hook-failed", "kernel_trap routing failed");
            return;
        }
        gTrapHookReady = true;
        AMDHVPublishState("diagnostic-trap-only", "trap forwarding only; no emulation");
        return;
    }
    updateHvSupport();
    if (!routeWithDiagnostics(patcher, KernelPatcher::KernelID, kernelRequests, arrsize(kernelRequests))) {
        SYSLOG("AMDHV", "Warning: Not all XNU kernel symbols were routed cleanly");
        patcher.clearError();
        gHookFailure = true;
        updateHvSupport();
        AMDHVPublishState("xnu-hook-failed", "hv_support=0");
    } else {
        SYSLOG("AMDHV", "Successfully routed XNU _hv_get_support, _host_vmxon, and _kernel_trap");
        gXnuReady = true;
        updateHvSupport();
        AMDHVPublishState(hooksReady() ? "active" : "xnu-hooked", "XNU hooks installed");
    }
}

void AMDHVPlugin::onKextLoad(void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size) {
    (void)user;
    if (gHookFailure || (!BackendPolicy::executionEnabled && gDiagnosticStage!=1)) return;
    if (id != kAppleHVKextInfo.loadIndex) {
        return;
    }

    if (!Compatibility::matchesImage(reinterpret_cast<const void *>(slide), size, Compatibility::applehvUUID)) {
        gHookFailure = true;
        AMDHVPublishState("unsupported-build", "AppleHV UUID or architecture mismatch");
        return;
    }
    if(!verifyInstructionContracts(patcher,id,false)) return;
    SYSLOG("AMDHV", "AppleHV.kext loaded (id=%zu), checking UUID and required symbols...", id);

    KernelPatcher::RouteRequest hvRequests[] = {
        {"_hv_vmx_vcpu_resume_or_launch_guest", hookedVcpuResumeOrLaunchGuest, orgVcpuResumeOrLaunchGuest},
        {"__ZN13hv_vmx_vcpu_t9init_vmcsEv",hookedVcpuInitVMCS,orgVcpuInitVMCS},
        {"__ZN13hv_vmx_vcpu_t4freeEv",hookedVcpuFree,orgVcpuFree},
        {"__ZN14hv_vmx_space_t10init_spaceEP11hv_vmx_vm_t",hookedSpaceInit,orgSpaceInit},
        {"__ZN14hv_vmx_space_t3mapEyymy",hookedSpaceMap,orgSpaceMap},
        {"__ZN14hv_vmx_space_t10map_kernelEyymi",hookedSpaceMapKernel,orgSpaceMapKernel},
        {"__ZN14hv_vmx_space_t9ept_faultEyyb",  hookedSpaceEptFault,           orgSpaceEptFault},
        {"__ZN14hv_vmx_space_t8_protectEymy",   hookedSpaceProtect,            orgSpaceProtect},
        {"__ZN14hv_vmx_space_t5unmapEym",       hookedSpaceUnmap,              orgSpaceUnmap},
        {"__ZN14hv_vmx_space_t10invalidateEv",  hookedSpaceInvalidate,         orgSpaceInvalidate},
        {"__ZN14hv_vmx_space_t4freeEv",         hookedSpaceFree,               orgSpaceFree},
    };

    if (!preflightRoutes(patcher, id, hvRequests, arrsize(hvRequests))) return;
    if (gDiagnosticStage == 1) {
        gApplePreflightComplete=true;publishPreflight();return;
    }
    if (!routeWithDiagnostics(patcher, id, hvRequests, arrsize(hvRequests), slide, size)) {
        SYSLOG("AMDHV", "Warning: Some AppleHV.kext symbols failed to route");
        patcher.clearError();
        gHookFailure = true;
        updateHvSupport();
        AMDHVPublishState("applehv-hook-failed", "hv_support=0");
    } else {
        SYSLOG("AMDHV", "All AppleHV.kext hooks installed successfully!");
        gAppleHVReady = true;
        updateHvSupport();
        AMDHVPublishState(hooksReady() ? "active" : "applehv-hooked", "AppleHV hooks installed");
    }
}

void AMDHVPlugin::hookedHvSupportInit() {
    updateHvSupport();
}

int AMDHVPlugin::hookedHvGetSupport() {
    return hooksReady() ? 1 : 0;
}

int AMDHVPlugin::hookedHostVmxon(bool exclusive) {
    if (!hooksReady()) return -1;
    return gHostLifecycle.acquire(exclusive)==HostLifecycle::Result::Success?0:-1;
}

void AMDHVPlugin::hookedHostVmxoff() {
    if(gHostLifecycle.release()!=HostLifecycle::Result::Success)
        AMDHVPublishState("backend-failed","host ownership/restore failed");
}

void AMDHVPlugin::hookedKernelTrap(XNUSavedState64 *state, uintptr_t *loSpp) {
    if(XNUPlatform::recoverTrap(state)) return;
    if (hooksReady() && SoftVMCS::handleKernelTrap(state)) {
        return;
    }
    if (orgKernelTrap) {
        FunctionCast(hookedKernelTrap, orgKernelTrap)(state, loSpp);
    }
}

int AMDHVPlugin::hookedVcpuResumeOrLaunchGuest(bool *isLaunched, AppleHVGPRFrame *gprs) {
    if (!hooksReady() || !isLaunched || !gprs) {
        return -1;
    }

    // Private AppleHV object access and a recoverable SVM transition have no
    // verified kernel adapter yet. Never reconstruct or dereference private
    // objects from an unchecked bool pointer, and never report a fake launch.
    return -1;
}

static bool snapshotSpace(AppleHVSpace *space,AppleHVABI::Space &snapshot) {
    MemoryAccess::Access memory{nullptr,
        [](void *,uint64_t a,void *p,size_t n) {return XNUPlatform::readOwnedKernel(a,p,n);},nullptr,nullptr};
    return AppleHVABI::readSpace(memory,reinterpret_cast<uint64_t>(space),snapshot);
}
bool AMDHVPlugin::hookedVcpuInitVMCS(void *vcpu) {
    uint64_t source=0;
    uint64_t owner=reinterpret_cast<uint64_t>(vcpu);
    if(!vcpu || !orgVcpuInitVMCS || !MemoryAccess::span(owner,0x394) || (owner&7) ||
       !XNUPlatform::readOwnedKernel(owner+0x30,&source,sizeof(source))) return false;
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(reinterpret_cast<AppleHVSpace *>(source),snapshot)) return false;
    auto key=SpaceLifecycle::global().lookup(snapshot);if(!key) return false;
    auto initialization=VcpuLifecycle::global().begin(owner,key);
    if(!initialization) return false;
    // The first VMCLEAR in the original function must claim its new VMCS via
    // the trap adapter while this scope still owns the initialization thread.
    return initialization.finish(FunctionCast(hookedVcpuInitVMCS,orgVcpuInitVMCS)(vcpu));
}
void AMDHVPlugin::hookedVcpuFree(void *vcpu) {
    if(!vcpu || !orgVcpuFree) {gHookFailure=true;return;}
    uint64_t owner=reinterpret_cast<uint64_t>(vcpu);
    if(VcpuLifecycle::global().untracked(owner)) {FunctionCast(hookedVcpuFree,orgVcpuFree)(vcpu);return;}
    auto retirement=VcpuLifecycle::global().retire(owner);
    if(!retirement) {gHookFailure=true;updateHvSupport();return;}
    FunctionCast(hookedVcpuFree,orgVcpuFree)(vcpu);
}
static constexpr int SpaceBackendError=static_cast<int>(0xfae94001U),SpaceArgumentError=static_cast<int>(0xfae94003U);
static bool spaceRange(uint64_t gpa,uint64_t size) {
    return size && !(gpa&4095) && !(size&4095) && gpa<(1ULL<<48) && size<=(1ULL<<48)-gpa;
}
bool AMDHVPlugin::hookedSpaceInit(AppleHVSpace *space,void *vm) {
    if(!space || !orgSpaceInit || !FunctionCast(hookedSpaceInit,orgSpaceInit)(space,vm)) return false;
    AppleHVABI::Space snapshot;
    return snapshotSpace(space,snapshot) && SpaceLifecycle::global().attach(snapshot)==SpaceLifecycle::Result::Success;
}
int AMDHVPlugin::hookedSpaceMap(AppleHVSpace *space,uint64_t hva,uint64_t gpa,unsigned long size,uint64_t flags) {
    if(!space || !orgSpaceMap || !spaceRange(gpa,size) || !MemoryAccess::span(hva,size)) return SpaceArgumentError;
    AppleHVABI::Space snapshot;if(!snapshotSpace(space,snapshot)) return SpaceBackendError;
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.unwire(gpa,size)!=SpaceLifecycle::Result::Success) return SpaceBackendError;
    // The original map calls our _protect hook. Both scopes share this ticket.
    // Leaf pinning is lazy: wire only pages actually faulted, never an entire VM.
    return FunctionCast(hookedSpaceMap,orgSpaceMap)(space,hva,gpa,size,flags);
}
int AMDHVPlugin::hookedSpaceMapKernel(AppleHVSpace *space,uint64_t hva,uint64_t gpa,unsigned long size,int protection) {
    if(!space || !orgSpaceMapKernel || !spaceRange(gpa,size) || !MemoryAccess::span(hva,size)) return SpaceArgumentError;
    AppleHVABI::Space snapshot;if(!snapshotSpace(space,snapshot)) return SpaceBackendError;
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.unwire(gpa,size)!=SpaceLifecycle::Result::Success) return SpaceBackendError;
    return FunctionCast(hookedSpaceMapKernel,orgSpaceMapKernel)(space,hva,gpa,size,protection);
}

int AMDHVPlugin::hookedSpaceEptFault(AppleHVSpace *space,uint64_t gpa,uint64_t info,bool vcpua) {
    if(!space || !orgSpaceEptFault || gpa>=(1ULL<<48)) return 3;
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(space,snapshot)) return 3;
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.unwire(gpa&~4095ULL,4096)!=SpaceLifecycle::Result::Success) return 3;
    int result=FunctionCast(hookedSpaceEptFault,orgSpaceEptFault)(space,gpa,info,vcpua);
    if(!result && mutation.pinFaultPage(gpa)!=SpaceLifecycle::Result::Success) return 3;
    return result;
}
int AMDHVPlugin::hookedSpaceProtect(AppleHVSpace *space,uint64_t gpa,unsigned long size,uint64_t flags) {
    if(!space || !orgSpaceProtect || !spaceRange(gpa,size)) return SpaceArgumentError;
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(space,snapshot)) return SpaceBackendError;
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.unwire(gpa,size)!=SpaceLifecycle::Result::Success) return SpaceBackendError;
    return FunctionCast(hookedSpaceProtect,orgSpaceProtect)(space,gpa,size,flags);
}
int AMDHVPlugin::hookedSpaceUnmap(AppleHVSpace *space,uint64_t gpa,unsigned long size) {
    if(!space || !orgSpaceUnmap || !spaceRange(gpa,size)) return SpaceArgumentError;
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(space,snapshot)) return SpaceBackendError;
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.unwire(gpa,size)!=SpaceLifecycle::Result::Success) return SpaceBackendError;
    return FunctionCast(hookedSpaceUnmap,orgSpaceUnmap)(space,gpa,size);
}
void AMDHVPlugin::hookedSpaceInvalidate(AppleHVSpace *space) {
    if(!space || !orgSpaceInvalidate) {gHookFailure=true;return;}
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(space,snapshot)) {gHookFailure=true;return;}
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation) {gHookFailure=true;updateHvSupport();AMDHVPublishState("backend-failed","source invalidation could not quiesce");return;}
    FunctionCast(hookedSpaceInvalidate,orgSpaceInvalidate)(space);
}
void AMDHVPlugin::hookedSpaceFree(AppleHVSpace *space) {
    if(!space || !orgSpaceFree) {gHookFailure=true;return;}
    if(SpaceLifecycle::global().untracked(reinterpret_cast<uint64_t>(space))) {
        // Failed/unsupported init can leave no vm_map or EPTP. Such an object
        // never had a shadow root, and its original cleanup must still execute.
        FunctionCast(hookedSpaceFree,orgSpaceFree)(space);return;
    }
    AppleHVABI::Space snapshot;
    if(!snapshotSpace(space,snapshot)) {gHookFailure=true;return;}
    auto mutation=SpaceLifecycle::global().begin(snapshot);
    if(!mutation || mutation.retire()!=SpaceLifecycle::Result::Success) {
        gHookFailure=true;updateHvSupport();AMDHVPublishState("backend-failed","source retirement could not quiesce");return;
    }
    FunctionCast(hookedSpaceFree,orgSpaceFree)(space);
}

// ============================================================================
// Lilu Plugin Configuration & Kext Entry Points
// ============================================================================
static const char *bootargOff[] = { "-amdhvoff" };
static const char *bootargDebug[] = { "-amdhvdbg" };
static const char *bootargBeta[] = { "-amdhvbeta" };

EXPORT PluginConfiguration ADDPR(config) = {
    "AMDHV",
    100,
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Tahoe,
    []() {
        gAMDHVPlugin.init();
    }
};

#if defined(AMDHV_KERNEL_BUILD)
#include <mach/mach_types.h>


class EXPORT AMDHV : public IOService {
    OSDeclareDefaultStructors(AMDHV)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual bool serializeProperties(OSSerialize *serialize) const override;
};

OSDefineMetaClassAndStructors(AMDHV, IOService)

bool AMDHV::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    if(!setProperty("AMDHVVersion","1.0.0") || !setProperty("AMDHVBuild",AMDHV_BUILD_ID)) {IOService::stop(provider);return false;}
    return true;
}

void AMDHV::stop(IOService *provider) {
    IOService::stop(provider);
}

bool AMDHV::serializeProperties(OSSerialize *serialize) const {
    char state[64] {},detail[128] {};
    {
        Runtime::Guard guard(gStateGate);
        if(!guard || gStateLost || gHookFailure) {snprintf(state,sizeof(state),"state-unavailable");}
        else {snprintf(state,sizeof(state),"%s",gState);snprintf(detail,sizeof(detail),"%s",gDetail);}
    }
    auto dictionary=dictionaryWithProperties();
    if(!dictionary) return false;
    auto stateString=OSString::withCString(state);
    auto detailString=OSString::withCString(detail);
    bool ok=stateString && detailString && dictionary->setObject("AMDHVState",stateString) &&
        dictionary->setObject("AMDHVDetail",detailString) && dictionary->serialize(serialize);
    if(stateString) stateString->release();
    if(detailString) detailString->release();
    dictionary->release();return ok;
}

extern "C" kern_return_t AMDHV_start(kmod_info_t *ki, void *d) {
    (void)ki; (void)d;
    AMDHVPublishState("bootstrap", "1.0.0");

    int offArg = 0;
    if (PE_parse_boot_argn("-amdhvoff", &offArg, sizeof(offArg))) {
        AMDHVPublishState("disabled", "-amdhvoff");
        return KERN_SUCCESS;
    }

    int dbgArg = 0;
    if (PE_parse_boot_argn("-amdhvdbg", &dbgArg, sizeof(dbgArg))) {
        amdhvDebugEnabled = true;
    }

    bool hasLegacyStage = PE_parse_boot_argn("amdhvstage", &gDiagnosticStage, sizeof(gDiagnosticStage));
    char modeName[24] {};
    if (PE_parse_boot_argn("amdhvmode", modeName, sizeof(modeName))) {
        modeName[sizeof(modeName)-1] = 0;
        BackendPolicy::Mode mode;
        if (!BackendPolicy::parseMode(modeName, mode) ||
            (hasLegacyStage && static_cast<int>(mode) != gDiagnosticStage)) {
            AMDHVPublishState("disabled", "invalid or conflicting diagnostic mode arguments");
            return KERN_SUCCESS;
        }
        gDiagnosticStage = static_cast<int>(mode);
    }
    if (gDiagnosticStage < 0 || gDiagnosticStage > 3) {
        AMDHVPublishState("disabled", "invalid amdhvstage (expected 0, 1, 2 or 3)");
        return KERN_SUCCESS;
    }
    if (gDiagnosticStage == 0) {
        gAMDHVPlugin.init();
        return KERN_SUCCESS;
    }

    LiluAPI::Error err = lilu.requestAccess();
    if (err != LiluAPI::Error::NoError) {
        char buf[64] {};
        snprintf(buf, sizeof(buf), "Lilu requestAccess returned %d", static_cast<int>(err));
        AMDHVPublishState("lilu-request-failed", buf);
        return KERN_SUCCESS;
    }

    err = lilu.shouldLoad(ADDPR(config).product, ADDPR(config).version,
        ADDPR(config).runmode, bootargOff, arrsize(bootargOff),
        bootargDebug, arrsize(bootargDebug), bootargBeta, arrsize(bootargBeta),
        ADDPR(config).minKernel, ADDPR(config).maxKernel, amdhvDebugEnabled);
    if (err != LiluAPI::Error::NoError) {
        lilu.releaseAccess();
        AMDHVPublishState("disabled", "Lilu compatibility check rejected load");
        return KERN_SUCCESS;
    }
    gAMDHVPlugin.init();
    lilu.releaseAccess();

    // Even failed registration may leave a callback owned by Lilu.
    return KERN_SUCCESS;
}

extern "C" kern_return_t AMDHV_stop(kmod_info_t *ki, void *d) {
    (void)ki; (void)d;
    if (gRegistrationAttempted) {
        return KERN_FAILURE;
    }
    gAMDHVPlugin.deinit();
    return KERN_SUCCESS;
}

extern "C" {
    kern_return_t _start(kmod_info_t *ki, void *data);
    kern_return_t _stop(kmod_info_t *ki, void *data);
    __attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.amdhv.AMDHV, "1.0.0", _start, _stop)
    __private_extern__ kmod_start_func_t *_realmain = AMDHV_start;
    __private_extern__ kmod_stop_func_t  *_antimain = AMDHV_stop;
    __private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
#endif
