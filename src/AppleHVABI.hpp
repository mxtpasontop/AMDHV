#ifndef AMDHV_APPLEHV_ABI_HPP
#define AMDHV_APPLEHV_ABI_HPP
#include <src/IntelVMX.hpp>
#include <src/MemoryAccess.hpp>

// Contracts inspected in the AppleHV UUID recorded in config/abi.json.
// Safe copying does not retain an OSObject: the caller must hold its lifetime.
namespace AppleHVABI {
constexpr size_t RWFrame=0xc80;
struct Vcpu {
    uint64_t address {}, vm {}, space {}, rw {}, ro {}, vmcsPA {}, guestDR6 {};
    uint32_t id {}, cpu {}, debugMode {};
    bool launched {};
};
struct Space {
    uint64_t address {}, map {}, pmap {}, task {}, rootPA {}, eptp {};
    uint32_t references {};
};
inline bool field(const MemoryAccess::Access &memory,uint64_t base,size_t offset,void *out,size_t size) {
    return offset<=UINT64_MAX-base && memory.load(base+offset,out,size);
}
template<class T> inline bool field(const MemoryAccess::Access &memory,uint64_t base,size_t offset,T &out) {
    return field(memory,base,offset,&out,sizeof(out));
}
inline bool readVcpu(const MemoryAccess::Access &memory,uint64_t launchedAddress,uint64_t frameAddress,Vcpu &out) {
    out={};
    if(launchedAddress<APPLEHV_VCPU_OFFSET_IS_LAUNCHED) return false;
    Vcpu v;
    v.address=launchedAddress-APPLEHV_VCPU_OFFSET_IS_LAUNCHED;
    if(!MemoryAccess::span(v.address,0x394) || (v.address&7)) return false;
    uint8_t launched=0;
    if(!field(memory,v.address,APPLEHV_VCPU_OFFSET_ID,v.id) ||
       !field(memory,v.address,APPLEHV_VCPU_OFFSET_VM,v.vm) ||
       !field(memory,v.address,APPLEHV_VCPU_OFFSET_SPACE,v.space) ||
       !field(memory,v.address,APPLEHV_VCPU_OFFSET_RW_IF,v.rw) ||
       !field(memory,v.address,APPLEHV_VCPU_OFFSET_RO_IF,v.ro) ||
       !field(memory,v.address,APPLEHV_VCPU_OFFSET_VMCS_PA,v.vmcsPA) ||
       !field(memory,v.address,0x120,v.cpu) || !field(memory,v.address,0x180,v.guestDR6) ||
       !field(memory,v.address,0x390,v.debugMode) || !memory.load(launchedAddress,&launched,1)) return false;
    if(launched>1 || !v.vm || !v.space || !v.ro || !v.rw || v.debugMode>2 ||
       v.rw>UINT64_MAX-RWFrame || v.rw+RWFrame!=frameAddress ||
       !MemoryAccess::span(frameAddress,sizeof(AppleHVGPRFrame)) ||
       !v.vmcsPA || (v.vmcsPA&~0x000ffffffffff000ULL)) return false;
    v.launched=launched;out=v;return true;
}
inline bool readSpace(const MemoryAccess::Access &memory,uint64_t address,Space &out) {
    out={};Space s;s.address=address;
    if(!address || (address&7) || !MemoryAccess::span(address,0x40)) return false;
    uint8_t ad=0,executeOnly=0,mbe=0;
    if(!field(memory,address,0x10,s.map) || !field(memory,address,0x18,s.rootPA) ||
       !field(memory,address,0x20,s.pmap) || !field(memory,address,0x30,s.task) ||
       !field(memory,address,0x38,s.references) || !field(memory,address,0x28,ad) ||
       !field(memory,address,0x29,executeOnly) || !field(memory,address,0x2a,mbe)) return false;
    // These optional EPT formats are not accepted by the current shadow backend.
    uint64_t flags=s.rootPA&~0x000ffffffffff000ULL;
    if(ad || executeOnly || mbe || (flags && flags!=0x1e) || !s.map || !s.pmap || !s.task || !(s.rootPA&0x000ffffffffff000ULL)) return false;
    s.rootPA&=0x000ffffffffff000ULL;
    s.eptp=s.rootPA|0x1e;out=s;return true;
}
}
#endif
