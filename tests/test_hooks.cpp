// Exercise real startup/routing code with a mock Lilu patcher. No privileged
// instruction, live kernel patch, or Hypervisor.framework call is executed.
#include <src/AMDHV.hpp>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

#define AMDHV_CPU_HPP
namespace CPUInfo {
static bool supported = true;
static void getCpuid(uint32_t leaf, uint32_t, uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
    a = b = c = d = 0;
    if(leaf==0x80000000U && supported) a=0x8000000aU;
    if(leaf==0 && supported) {b=0x68747541;d=0x69746e65;c=0x444d4163;}
    if (leaf == 0x80000001U && supported) c = 1U << 2;
    if (leaf == 0x8000000AU && supported) { b = 64; d = 9; }
}
}
#include <src/AMDHV.cpp>

LiluAPI lilu;
static LiluAPI::Error registrationError = LiluAPI::NoError,patcherRegistrationError=LiluAPI::NoError;
static unsigned registrations;
static bool badKernelUUID=false;
static LiluAPI::t_patcherLoaded patcherCallback;
static LiluAPI::t_kextLoaded kextCallback;
static const char *missingSymbol;
static bool routeFailure;
static std::vector<std::string> routed;
LiluAPI::Error LiluAPI::onPatcherLoad(t_patcherLoaded cb, void *) {
    ++registrations;patcherCallback = cb;
    return patcherRegistrationError;
}
LiluAPI::Error LiluAPI::onKextLoad(KernelPatcher::KextInfo *, size_t, t_kextLoaded cb, void *) {
    ++registrations;kextCallback = cb;
    return registrationError;
}
static uint64_t physmap;
static int hvSupport;
mach_vm_address_t KernelPatcher::solveSymbol(size_t, const char *name) {
    if (missingSymbol && !strcmp(name, missingSymbol)) return 0;
    static unsigned char badUUID[16] {};
    if(!strcmp(name,"_kernel_uuid") && badKernelUUID) return reinterpret_cast<mach_vm_address_t>(badUUID);
    if (!strcmp(name, "_kernel_uuid")) return reinterpret_cast<mach_vm_address_t>(Compatibility::kernelUUID);
    if (!strcmp(name, "_physmap_base")) return reinterpret_cast<mach_vm_address_t>(&physmap);
    if (!strcmp(name, "_hv_support_available")) return reinterpret_cast<mach_vm_address_t>(&hvSupport);
    return 0x1000;
}
void KernelPatcher::clearError() {}
static int originalTrapCalls;
static XNUSavedState64 *forwardedTrap;
static void (*trapDetour)(XNUSavedState64 *, uintptr_t *);
static uintptr_t *forwardedStack;
static void originalTrap(XNUSavedState64 *state, uintptr_t *stack) {
    forwardedStack = stack;
    ++originalTrapCalls;
    forwardedTrap = state;
}
bool KernelPatcher::routeMultiple(size_t, RouteRequest *requests, size_t count,
                                 mach_vm_address_t, size_t, bool, bool) {
    for (size_t i = 0; i < count; ++i) {
        routed.emplace_back(requests[i].symbol);
        if (!routeFailure && !strcmp(requests[i].symbol, "_kernel_trap")) {
            if(!requests[i].org) abort();
            *requests[i].org = reinterpret_cast<mach_vm_address_t>(&originalTrap);
            trapDetour = reinterpret_cast<void (*)(XNUSavedState64 *, uintptr_t *)>(requests[i].to);
        }
    }
    return !routeFailure;
}

static void reset() {
    gXnuReady = gAppleHVReady = gHookFailure = false;
    gRegistrationAttempted = gCallbacksRegistered = false;
    gDiagnosticStage = 2;
    gTrapHookReady = false;
    gXnuPreflightComplete=false;gApplePreflightComplete=false;
    registrations=0;badKernelUUID=false;
    patcherCallback=nullptr;kextCallback=nullptr;
    patcherRegistrationError=LiluAPI::NoError;
    orgKernelTrap = 0;
    originalTrapCalls = 0;
    forwardedTrap = nullptr;
    trapDetour = nullptr;
    gVmxHvSupportPtr = nullptr;
    hvSupport = 0;
    missingSymbol = nullptr;
    routeFailure = false;
    registrationError = LiluAPI::NoError;
    CPUInfo::supported = true;
    routed.clear();
    kAppleHVKextInfo.loadIndex = 1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed: %s at %d\n", #x, __LINE__); abort(); } } while (0)
#include "SpaceHookFixture.hpp"
int main() {
    CHECK(amd_svm_vcpu_run(0,0,nullptr)==-1);
    KernelPatcher patcher;
    for (int mode : {2, 3}) {
        reset();
        gDiagnosticStage = mode;
        patcherCallback = nullptr;
        kextCallback = nullptr;
        CHECK(!gAMDHVPlugin.init());
        CHECK(gHookFailure && !hooksReady());
        CHECK(!patcherCallback && !kextCallback && routed.empty());
    }
    reset();
    gDiagnosticStage = 0;
    patcherCallback = nullptr;
    kextCallback = nullptr;
    CHECK(gAMDHVPlugin.init());
    CHECK(!patcherCallback && !kextCallback);
    reset();
    gDiagnosticStage = 1;
    CHECK(gAMDHVPlugin.init());
    hvSupport = 7;
    patcherCallback(nullptr, patcher);
    unsigned char image[56] {};
    auto put=[&](unsigned offset,uint32_t value) { for(unsigned i=0;i<4;++i) image[offset+i]=value>>(8*i); };
    put(0,0xfeedfacf);put(4,0x01000007);put(16,1);put(20,24);put(32,0x1b);put(36,24);
    memcpy(image+40,Compatibility::applehvUUID,16);
    kextCallback(nullptr, patcher, 1, reinterpret_cast<mach_vm_address_t>(image), sizeof(image));
    CHECK(!gHookFailure);
    CHECK(!Compatibility::matchesImage(image,55,Compatibility::applehvUUID));
    image[40]^=1;
    CHECK(!Compatibility::matchesImage(image,sizeof(image),Compatibility::applehvUUID));
    CHECK(routed.empty() && hvSupport == 7 && !hooksReady());
    CHECK(gXnuPreflightComplete && gApplePreflightComplete);
    CHECK(gAMDHVPlugin.init() && registrations==2); // No duplicate callbacks.
    reset();gDiagnosticStage=1;
    patcherRegistrationError=static_cast<LiluAPI::Error>(1);
    CHECK(!gAMDHVPlugin.init() && gRegistrationAttempted && gHookFailure);
    CHECK(!gAMDHVPlugin.init() && registrations==2); // Partial registration pins lifetime.
    reset();gDiagnosticStage=1;registrationError=static_cast<LiluAPI::Error>(1);
    CHECK(!gAMDHVPlugin.init() && registrations==1 && gHookFailure);
    for(const char *symbol:{"_kernel_trap","_physmap_base","_hv_support_available"}) {
        reset();gDiagnosticStage=1;CHECK(gAMDHVPlugin.init());missingSymbol=symbol;
        patcherCallback(nullptr,patcher);CHECK(gHookFailure && routed.empty());
    }
    reset();gDiagnosticStage=1;CHECK(gAMDHVPlugin.init());badKernelUUID=true;
    patcherCallback(nullptr,patcher);CHECK(gHookFailure && routed.empty());
    reset();gDiagnosticStage=1;CHECK(gAMDHVPlugin.init());CPUInfo::supported=false;
    patcherCallback(nullptr,patcher);CHECK(gHookFailure && routed.empty());
    reset();gDiagnosticStage=1;CHECK(gAMDHVPlugin.init());
    image[40]^=1; // Restore supported AppleHV UUID.
    kextCallback(nullptr,patcher,1,reinterpret_cast<mach_vm_address_t>(image),sizeof(image));
    CHECK(gApplePreflightComplete && !gXnuPreflightComplete && !strcmp(gState,"diagnostic-waiting-xnu"));
    patcherCallback(nullptr,patcher);
    CHECK(!strcmp(gState,"diagnostic-applehv-preflight"));
    // Independent two-argument ABI: a deliberately nonnull second argument
    // must reach the original handler unchanged even in full-mode forwarding.
    orgKernelTrap = reinterpret_cast<mach_vm_address_t>(&originalTrap);
    gDiagnosticStage = 2;
    XNUSavedState64 state{};
    uintptr_t stack = 0x1234;
    AMDHVPlugin::forwardTrapForTest(&state, &stack);
    CHECK(originalTrapCalls == 1 && forwardedTrap == &state && forwardedStack == &stack);
    KernelPatcher::RouteRequest trapRequest{"_kernel_trap",AMDHVPlugin::forwardTrapForTest,orgKernelTrap};
    CHECK(routeWithDiagnostics(patcher,KernelPatcher::KernelID,&trapRequest,1) && gTrapHookReady);
    spaceHookIntegration();
    puts("Startup, two-argument trap ABI, nested AppleHV hooks and source-lifetime integration tests passed.");
}
