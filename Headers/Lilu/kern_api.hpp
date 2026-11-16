#if defined(AMDHV_KERNEL_BUILD)
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#else
// Userspace test double; never used to compile the kext.
#ifndef LILU_KERN_API_HPP
#define LILU_KERN_API_HPP

#include <Headers/Lilu/kern_patcher.hpp>
#include <Headers/Lilu/kern_util.hpp>

struct PluginConfiguration {
    const char *product;
    size_t version;
    uint32_t runmode;
    const char **disableArg;
    size_t disableArgNum;
    const char **debugArg;
    size_t debugArgNum;
    const char **betaArg;
    size_t betaArgNum;
    KernelVersion minKernel;
    KernelVersion maxKernel;
    void (*pluginStart)();
};

class LiluAPI {
public:
    enum RunningModes : uint32_t {
        AllowNormal     = 1 << 0,
        AllowInstaller  = 1 << 1,
        AllowSafeMode   = 1 << 2,
    };

    enum Error {
        NoError             = 0,
        LockError           = 1,
        MemoryError         = 2,
        UnsupportedFeature  = 3,
        IncompatibleOS      = 4,
        Disabled            = 5,
        TooLate             = 6,
    };

    using t_patcherLoaded = void (*)(void *user, KernelPatcher &patcher);
    using t_kextLoaded    = void (*)(void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size);

    Error requestAccess(size_t version = 120, bool check = false);
    Error releaseAccess();
    Error shouldLoad(const char *product, size_t version, uint32_t runmode, const char **disableArg, size_t disableArgNum, const char **debugArg, size_t debugArgNum, const char **betaArg, size_t betaArgNum, KernelVersion min, KernelVersion max, bool &printDebug);
    Error onPatcherLoad(t_patcherLoaded callback, void *user = nullptr);
    Error onKextLoad(KernelPatcher::KextInfo *infos, size_t num = 1, t_kextLoaded callback = nullptr, void *user = nullptr);

    inline void onPatcherLoadForce(t_patcherLoaded callback, void *user = nullptr) {
        onPatcherLoad(callback, user);
    }

    inline void onKextLoadForce(KernelPatcher::KextInfo *infos, size_t num = 1, t_kextLoaded callback = nullptr, void *user = nullptr) {
        onKextLoad(infos, num, callback, user);
    }
};

extern LiluAPI lilu;

#endif // LILU_KERN_API_HPP

#endif
