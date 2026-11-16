#ifndef AMDHV_MEMORY_HPP
#define AMDHV_MEMORY_HPP
#include <stddef.h>
#include <stdint.h>
namespace Memory {
// Setup/teardown only. Never call from a trap, rendezvous, or guest exit.
void *allocate(size_t bytes, uint64_t &physical);
void release(void *address, size_t bytes);
#ifndef AMDHV_KERNEL_BUILD
// -1 disables injection; zero fails the next and all subsequent allocations.
void failAfter(int allocations);
size_t outstanding();
#endif
}
#endif
