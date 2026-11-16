#ifndef AMDHV_HPP
#define AMDHV_HPP

#include <Headers/Lilu/kern_api.hpp>
#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>

extern "C" int amd_svm_vcpu_run(uint64_t vmcbPA, uint64_t hsavePA, void *gprs);

class AMDHVPlugin {
public:
    bool init();
    void deinit();
#ifndef AMDHV_KERNEL_BUILD
    static void forwardTrapForTest(XNUSavedState64 *state, uintptr_t *stack) { hookedKernelTrap(state, stack); }
    friend struct AMDHVHookTestAccess;
#endif

    static bool detectAMDSVM(uint32_t &outMaxASID, bool &outHasNP, bool &outHasNRIPS);

private:
    static void onPatcherLoad(void *user, KernelPatcher &patcher);
    static void onKextLoad(void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size);

    // XNU Kernel Hook Callbacks
    static void hookedHvSupportInit();
    static int  hookedHvGetSupport();
    static int  hookedHostVmxon(bool exclusive);
    static void hookedHostVmxoff();
    static void hookedKernelTrap(XNUSavedState64 *state, uintptr_t *loSpp);

    // AppleHV.kext Hook Callbacks
    static int  hookedVcpuResumeOrLaunchGuest(bool *isLaunched, AppleHVGPRFrame *gprs);
    static bool hookedVcpuInitVMCS(void *vcpu);
    static void hookedVcpuFree(void *vcpu);
    static bool hookedSpaceInit(AppleHVSpace *space,void *vm);
    static int hookedSpaceMap(AppleHVSpace *space,uint64_t hva,uint64_t gpa,unsigned long size,uint64_t flags);
    static int hookedSpaceMapKernel(AppleHVSpace *space,uint64_t hva,uint64_t gpa,unsigned long size,int protection);
    static int  hookedSpaceEptFault(AppleHVSpace *space, uint64_t gpa, uint64_t faultInfo, bool isVcpua);
    static int  hookedSpaceProtect(AppleHVSpace *space, uint64_t gpa, unsigned long size, uint64_t flags);
    static int  hookedSpaceUnmap(AppleHVSpace *space, uint64_t gpa, unsigned long size);
    static void hookedSpaceInvalidate(AppleHVSpace *space);
    static void hookedSpaceFree(AppleHVSpace *space);
};

#endif // AMDHV_HPP
