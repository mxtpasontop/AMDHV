#if defined(AMDHV_KERNEL_BUILD)
#include <Headers/kern_patcher.hpp>
#else
// Userspace test double; never used to compile the kext.
#ifndef LILU_KERN_PATCHER_HPP
#define LILU_KERN_PATCHER_HPP

#include <Headers/Lilu/kern_util.hpp>
#include <mach/vm_types.h>

class KernelPatcher {
public:
    enum Error {
        NoError         = 0,
        NoKinfoFound    = 1,
        NoSymbolFound   = 2,
        KernInitFailure = 3,
        MemoryIssue     = 4,
        DisasmFailure   = 5,
    };

    struct KextInfo {
        enum SysFlags : uint64_t {
            Loaded      = 0,
            Reloadable  = 1,
            Disabled    = 2,
            FSOnly      = 3,
            FSFallback  = 4,
            Reserved    = 5,
            SysFlagNum  = 6,
        };
        enum UserFlags : size_t {
            Reserved1   = 0,
            Reserved2   = 1,
            UserFlagNum = 2,
        };
        static constexpr size_t Unloaded = 0;

        const char *id;
        const char **paths;
        size_t pathNum;
        bool sys[SysFlagNum];
        bool user[UserFlagNum];
        size_t loadIndex;

        void switchOff() {
            sys[Disabled] = true;
        }
    };

    static_assert(sizeof(KextInfo) == 40, "Lilu KextInfo ABI size");
    static_assert(offsetof(KextInfo, loadIndex) == 32, "Lilu loadIndex ABI offset");

    struct RouteRequest {
        const char *symbol {nullptr};
        mach_vm_address_t from {0};
        mach_vm_address_t to {0};
        mach_vm_address_t *org {nullptr};

        template <typename T, typename Y>
        RouteRequest(const char *s, T t, Y &o) :
            symbol(s),
            to(reinterpret_cast<mach_vm_address_t>(t)),
            org(reinterpret_cast<mach_vm_address_t *>(&o)) {}

        template <typename T>
        RouteRequest(const char *s, T t) :
            symbol(s),
            to(reinterpret_cast<mach_vm_address_t>(t)),
            org(nullptr) {}
    };

    static constexpr size_t KernelID = 0;

    Error getError();
    void clearError();
    mach_vm_address_t solveSymbol(size_t id, const char *symbol);
    mach_vm_address_t routeFunction(mach_vm_address_t from, mach_vm_address_t to, bool buildWrapper = true, bool kernelRoute = true, bool revertible = true);
    bool routeMultiple(size_t id, RouteRequest *requests, size_t num, mach_vm_address_t start = 0, size_t size = 0, bool kernelRoute = true, bool force = false);
};

#endif // LILU_KERN_PATCHER_HPP

#endif
