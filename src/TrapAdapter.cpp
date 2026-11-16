#include <src/TrapAdapter.hpp>
#include <src/InstructionDecoder.hpp>
namespace TrapAdapter {
static Adapter instance;
Adapter &global() {return instance;}
namespace {
bool contains(uint64_t first,size_t size,uint64_t address,size_t bytes) {
    return first && MemoryAccess::span(first,size) && MemoryAccess::span(address,bytes) && address>=first &&
        address-first<=size && bytes<=size-(address-first);
}
struct Session {
    const Operations &ops;
    Code code;
    VcpuLifecycle::Buffer stack;
    VcpuLifecycle::Registry::Lease current,region;
    bool owns(uint64_t address,size_t size,bool write) const {
        if(contains(stack.address,stack.bytes,address,size) || current.owns(address,size,write) || region.owns(address,size,write)) return true;
        return !write && (contains(code.address,code.bytes,address,size) || ops.readOnly(ops.context,code,address,size));
    }
    bool physical(uint64_t pa,void *out,size_t size) {
        if(size!=4 || !out || !pa || (pa&~NPTManager::PHYS_ADDR_MASK)) return false;
        if(current && current.physical()==pa) return current.readRevision(pa,out,size);
        if(!region || region.physical()!=pa) region=ops.vcpus->acquirePhysical(pa);
        return region && region.readRevision(pa,out,size);
    }
};
}
bool Adapter::configure(Operations operations) {
    Runtime::Guard guard(control_);
    auto &f=operations.faults;
    if(!guard || configured_ || !operations.vcpus || !operations.pinCode || !operations.unpinCode || !operations.readOnly ||
       !operations.stack || !operations.readyCPU || !f.cpu || !f.thread || !f.disableInterrupts || !f.restoreInterrupts || !f.probeInstalled) return false;
    ops_=operations;configured_=true;return true;
}
bool Adapter::handle(XNUSavedState64 *state) {
    if(!configured_ || !state || state->flavor!=15 || (state->cs&3) ||
       (state->trapno!=XNU_TRAP_INVALID_OPCODE && state->trapno!=XNU_TRAP_GENERAL_PROTECTION) || !MemoryAccess::span(state->rip,1)) return false;
    auto &f=ops_.faults;
    if(!f.probeInstalled(f.context)) return false;
    bool previous=f.disableInterrupts(f.context);
    struct Interrupts {const FaultMemory::Context &f;bool previous;~Interrupts() {f.restoreInterrupts(f.context,previous);}} interrupts{f,previous};
    uint32_t cpu=f.cpu(f.context);uintptr_t thread=f.thread(f.context);
    if(cpu>=128 || !thread || __atomic_test_and_set(&active_[cpu],__ATOMIC_ACQUIRE)) return false;
    struct Admission {uint8_t &flag;~Admission() {__atomic_clear(&flag,__ATOMIC_RELEASE);}} admission{active_[cpu]};
    Session session{ops_,{},{},{},{}};
    if(!ops_.pinCode(ops_.context,state->rip,session.code)) return false;
    struct Image {const Operations &ops;const Code &code;~Image() {ops.unpinCode(ops.context,code);}} image{ops_,session.code};
    if(!session.code.token || !contains(session.code.address,session.code.bytes,state->rip,1)) return false;
    if(!ops_.stack(ops_.context,thread,session.stack) || !contains(session.stack.address,session.stack.bytes,state->rsp,1)) session.stack={};
    // Retain the AppleHV lifetime ticket for register-only VMREAD/VMWRITE too.
    // A snapshot distinguishes an unbound CPU from a contended mutable lease.
    {
        SoftVMCS::ActiveIdentity current;
        if(!SoftVMCS::snapshotActive(cpu,current)) return false;
        if(current.physical) {
            session.current=ops_.vcpus->acquire(current.owner);
            if(!session.current || session.current.lifetime()!=current.lifetime || session.current.physical()!=current.physical) return false;
        }
    }
    FaultMemory::Context owned;
    owned.context=&session;
    owned.cpu=[](void *p) {auto &s=*static_cast<Session *>(p);return s.ops.faults.cpu(s.ops.faults.context);};
    owned.thread=[](void *p) {auto &s=*static_cast<Session *>(p);return s.ops.faults.thread(s.ops.faults.context);};
    owned.disableInterrupts=[](void *p) {auto &s=*static_cast<Session *>(p);return s.ops.faults.disableInterrupts(s.ops.faults.context);};
    owned.restoreInterrupts=[](void *p,bool value) {auto &s=*static_cast<Session *>(p);s.ops.faults.restoreInterrupts(s.ops.faults.context,value);};
    owned.probeInstalled=[](void *p) {auto &s=*static_cast<Session *>(p);return s.ops.faults.probeInstalled(s.ops.faults.context);};
    owned.owns=[](void *p,uint64_t a,size_t n,bool write) {return static_cast<Session *>(p)->owns(a,n,write);};
    FaultMemory::Adapter memory(owned);
    uint8_t bytes[15];size_t count=0;
    while(count<sizeof(bytes) && state->rip<=UINT64_MAX-count && contains(session.code.address,session.code.bytes,state->rip+count,1)) {
        if(!memory.read(state->rip+count,bytes+count,1)) return false;
        ++count;
    }
    InstructionDecoder::DecodedInstruction instruction;
    if(!InstructionDecoder::decodeInstruction(bytes,count,state,instruction)) return false;
    bool msr=instruction.op==InstructionDecoder::DecodedOp::Rdmsr || instruction.op==InstructionDecoder::DecodedOp::Wrmsr;
    if(!msr && !ops_.readyCPU(ops_.context,cpu)) return false;
    // FaultMemory uses the session as its ownership context; keep the adapter
    // callbacks behind one wrapper instead of replacing that context pointer.
    struct Access {FaultMemory::Adapter &memory;Session &session;} bridge{memory,session};
    MemoryAccess::Access access{&bridge,
        [](void *p,uint64_t a,void *out,size_t n) {return static_cast<Access *>(p)->memory.read(a,out,n);},
        [](void *p,uint64_t a,const void *in,size_t n) {return static_cast<Access *>(p)->memory.write(a,in,n);},
        [](void *p,uint64_t pa,void *out,size_t n) {return static_cast<Access *>(p)->session.physical(pa,out,n);}};
    return SoftVMCS::emulateInstruction(state,bytes,count,access,cpu,true);
}
}
