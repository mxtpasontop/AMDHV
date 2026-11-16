#include <src/SVMTransition.hpp>
#include <src/MemoryAccess.hpp>
#include <src/BackendPolicy.hpp>
#include <Headers/Lilu/kern_util.hpp>
namespace SVMTransition {
constexpr uint64_t Ready=0x414d44485653564dULL;
bool initialize(Context &c,VMCB *guestVA,uint64_t guestPA,uint64_t hostAuxPA,AppleHVGPRFrame *frame,uint16_t kernelCS) {
    c.ready=0;
    uint64_t address=reinterpret_cast<uint64_t>(&c),guest=reinterpret_cast<uint64_t>(guestVA),gpr=reinterpret_cast<uint64_t>(frame);
    if((address&4095) || !guest || (guest&4095) || !frame || !MemoryAccess::span(address,sizeof(c)) ||
       !MemoryAccess::span(guest,4096) || !MemoryAccess::span(gpr,sizeof(*frame)) ||
       !guestPA || (guestPA&~0x000ffffffffff000ULL) || !hostAuxPA || (hostAuxPA&~0x000ffffffffff000ULL) ||
       guestPA==hostAuxPA || !kernelCS || (kernelCS&7) ||
       (guest<address+sizeof(c) && address<guest+4096) || (gpr<address+sizeof(c) && address<gpr+sizeof(*frame)) ||
       (gpr<guest+4096 && guest<gpr+sizeof(*frame))) return false;
    memset(&c,0,sizeof(c));c.guestVA=guestVA;c.guestPA=c.runPA=guestPA;c.hostPA=hostAuxPA;c.frame=frame;
    c.privateIDTR.limit=4095;c.privateIDTR.base=reinterpret_cast<uint64_t>(c.idt);
    uint64_t table=reinterpret_cast<uint64_t>(amdhv_svm_trap_offsets);
    for(unsigned i=0;i<256;++i) {
        int64_t offset=amdhv_svm_trap_offsets[i];
        uint64_t distance=static_cast<uint64_t>(offset<0?-offset:offset);
        if((offset<0 && distance>table) || (offset>=0 && distance>UINT64_MAX-table)) return false;
        uint64_t handler=offset<0?table-distance:table+distance;
        if(!handler || !MemoryAccess::canonical(handler)) return false;
        c.idt[i]={uint16_t(handler),kernelCS,0,0x8e,uint16_t(handler>>16),uint32_t(handler>>32),0};
    }
    c.ready=Ready;return true;
}
Result run(Context &c) {
#if defined(AMDHV_KERNEL_BUILD)
    if(!BackendPolicy::faultSafeTransition || !BackendPolicy::executionEnabled) return Result::Unavailable;
    if(c.ready!=Ready || c.runPA!=c.guestPA) return Result::Unavailable;
    auto result=static_cast<Result>(amdhv_svm_transition_raw(&c));
    if(result==Result::FatalEvent) panic("AMDHV: transition event %llu rip=%llx error=%llx address=%llx",c.vector,c.faultRIP,c.error,c.faultAddress);
    return result;
#else
    (void)c;return Result::Unavailable;
#endif
}
}
