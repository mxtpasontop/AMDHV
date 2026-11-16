#ifndef AMDHV_FAULT_MEMORY_HPP
#define AMDHV_FAULT_MEMORY_HPP
#include <src/MemoryAccess.hpp>
#include <src/TrapRecovery.hpp>
namespace FaultMemory {
struct Context {
    void *context {};
    uint32_t (*cpu)(void *) {};
    uintptr_t (*thread)(void *) {};
    bool (*disableInterrupts)(void *) {}; // returns previous IF
    void (*restoreInterrupts)(void *,bool) {};
    bool (*probeInstalled)(void *) {};
    bool (*owns)(void *,uint64_t,size_t,bool write) {};
};
class Adapter {
    Context context_;
public:
    explicit Adapter(Context context):context_(context) {}
    MemoryAccess::Access access();
    bool read(uint64_t address,void *out,size_t size);
    bool write(uint64_t address,const void *in,size_t size);
};
}
#endif
