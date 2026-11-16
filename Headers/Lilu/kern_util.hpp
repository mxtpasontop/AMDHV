#if defined(AMDHV_KERNEL_BUILD)
#include <Headers/kern_util.hpp>
#define amdhvDebugEnabled AMDHV_debugEnabled
#else
#ifndef LILU_KERN_UTIL_HPP
#define LILU_KERN_UTIL_HPP

#include <stdint.h>
#include <stddef.h>

#if defined(AMDHV_KERNEL_BUILD)
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#define EXPORT __attribute__((visibility("default")))
#define NOINLINE __attribute__((noinline))
#define PACKED __attribute__((packed))

#if defined(AMDHV_KERNEL_BUILD)
#define SYSLOG(module, fmt, ...) IOLog(module ": " fmt "\n", ##__VA_ARGS__)
#define DBGLOG(module, fmt, ...) do { if (amdhvDebugEnabled) IOLog(module " [DBG]: " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#define SYSLOG(module, fmt, ...) printf(module ": " fmt "\n", ##__VA_ARGS__)
#define DBGLOG(module, fmt, ...) do { if (amdhvDebugEnabled) printf(module " [DBG]: " fmt "\n", ##__VA_ARGS__); } while (0)
#endif

extern bool amdhvDebugEnabled;

enum KernelVersion {
    Monterey    = 21,
    Ventura     = 22,
    Sonoma      = 23,
    Sequoia     = 24,
    Tahoe       = 25,
};

enum KernelMinorVersion {
     KernelMinorAny = 0
};

template <typename T, typename Y>
inline T FunctionCast(T, Y ptr) {
    return reinterpret_cast<T>(ptr);
}

#define arrsize(x) (sizeof(x) / sizeof((x)[0]))
#define ADDPR(x) amdhv_##x

#endif // LILU_KERN_UTIL_HPP

#endif
