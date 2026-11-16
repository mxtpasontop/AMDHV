#include <src/Memory.hpp>
#if defined(AMDHV_KERNEL_BUILD)
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#else
#include <cstdlib>
#include <cstring>
static int budget = -1;
static size_t live = 0;
#endif
namespace Memory {
void *allocate(size_t bytes, uint64_t &physical) {
    physical = 0;
    if (!bytes || bytes % 4096) return nullptr;
#if defined(AMDHV_KERNEL_BUILD)
    if (bytes>SIZE_MAX-4096) return nullptr;
    // Keep the owner descriptor in a private header page. The returned pages
    // remain wired and physically contiguous for the full allocation lifetime.
    auto descriptor=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIOMemoryPhysicallyContiguous|kIODirectionInOut,bytes+4096,4096);
    if(!descriptor) return nullptr;
    auto base=static_cast<uint8_t *>(descriptor->getBytesNoCopy());
    IOByteCount segment=0;
    uint64_t pa=descriptor->getPhysicalSegment(0,&segment,kIOMemoryMapperNone);
    if(!base || !pa || (pa&4095) || (reinterpret_cast<uintptr_t>(base)&4095) || segment<bytes+4096 || pa>UINT64_MAX-bytes-4096) {
        descriptor->release();return nullptr;
    }
    *reinterpret_cast<IOBufferMemoryDescriptor **>(base)=descriptor;
    void *p=base+4096;
    physical=pa+4096;
#else
    int remaining = __atomic_load_n(&budget, __ATOMIC_RELAXED);
    while (remaining >= 0) {
        if (!remaining) return nullptr;
        if (__atomic_compare_exchange_n(&budget, &remaining, remaining-1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
    }
    void *p = nullptr;
    if (posix_memalign(&p, 4096, bytes)) return nullptr;
    physical = reinterpret_cast<uint64_t>(p);
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
#endif
    memset(p, 0, bytes);
    return p;
}
void release(void *address, size_t bytes) {
    if (!address) return;
#if defined(AMDHV_KERNEL_BUILD)
    (void)bytes;
    auto base=static_cast<uint8_t *>(address)-4096;
    auto descriptor=*reinterpret_cast<IOBufferMemoryDescriptor **>(base);
    descriptor->release();
#else
    (void)bytes;
    free(address);
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
#endif
}
#ifndef AMDHV_KERNEL_BUILD
void failAfter(int allocations) { __atomic_store_n(&budget, allocations, __ATOMIC_RELAXED); }
size_t outstanding() { return __atomic_load_n(&live, __ATOMIC_RELAXED); }
#endif
}
