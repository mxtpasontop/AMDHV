#ifndef AMDHV_CPU_HPP
#define AMDHV_CPU_HPP

#include <stdint.h>

namespace CPUInfo {
    static inline void getCpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx) {
#if defined(__x86_64__)
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf), "c"(subleaf));
#else
        (void)leaf; (void)subleaf;
        eax = ebx = ecx = edx = 0;
#endif
    }
}

#endif // AMDHV_CPU_HPP
