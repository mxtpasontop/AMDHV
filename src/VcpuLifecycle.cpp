#include <src/VcpuLifecycle.hpp>
#include <src/AppleHVABI.hpp>
namespace VcpuLifecycle {
static Registry instance;
Registry &global() {return instance;}
static bool contains(Buffer buffer,uint64_t address,size_t bytes) {
    return buffer.address && MemoryAccess::span(buffer.address,buffer.bytes) && MemoryAccess::span(address,bytes) &&
        address>=buffer.address && address-buffer.address<=buffer.bytes && bytes<=buffer.bytes-(address-buffer.address);
}
static bool overlap(Buffer a,Buffer b) {
    return a.address<=b.address ? b.address-a.address<a.bytes : a.address-b.address<b.bytes;
}
static bool validPA(uint64_t pa) {return pa && !(pa&~NPTManager::PHYS_ADDR_MASK);}
bool Registry::configure(Operations operations) {
    Runtime::Guard guard(gate_);
    if(!guard || configured_ || !operations.memory.read || !operations.thread || !operations.retain ||
       !operations.release || !operations.inspect) return false;
    ops_=operations;configured_=true;return true;
}
bool Registry::ready() {Runtime::Guard guard(gate_);return guard && configured_;}
void Registry::reap() {
    for(auto &r:records_) if(r.state==Retired) {
        // Retirement owns all callbacks until it publishes Retired. No lease
        // can remain and no other thread can reuse its storage before here.
        r.owner=r.lifetime=r.vm=r.rw=r.ro=r.pa=0;r.thread=0;r.space={};r.core=false;
        for(unsigned i=0;i<3;++i) {r.descriptors[i]=0;r.buffers[i]={};}
        r.state=Empty;
    }
}
Registry::Initialization Registry::begin(uint64_t owner,const NPTManager::SpaceKey &space) {
    Runtime::Guard guard(gate_);
    if(!guard || !configured_ || !space || !owner || !MemoryAccess::span(owner,0x394) || (owner&7) || lifetime_==UINT64_MAX) return {};
    reap();uintptr_t thread=ops_.thread(ops_.context);if(!thread) return {};
    Record *free=nullptr;
    for(auto &r:records_) {
        if(r.state!=Empty && (r.owner==owner || (r.state==Initializing && r.thread==thread))) return {};
        if(r.state==Empty && !free) free=&r;
    }
    if(!free) return {};
    uint64_t vm=0,source=0,rw=0,ro=0,descriptors[2] {};
    auto &m=ops_.memory;
    if(!AppleHVABI::field(m,owner,0x28,vm) || !AppleHVABI::field(m,owner,0x30,source) || source!=space.owner() ||
       !AppleHVABI::field(m,owner,0x38,rw) || !AppleHVABI::field(m,owner,0x40,ro) ||
       !vm || !rw || !ro || !MemoryAccess::span(vm,0x288) || !AppleHVABI::field(m,vm,0x280,descriptors[0]) ||
       !AppleHVABI::field(m,vm,0x270,descriptors[1]) || !descriptors[0] || !descriptors[1]) return {};
    auto hold=NPTManager::retainSpace(space);if(!hold) return {};
    // Publish cleanup identity before retaining descriptors. Every subsequent
    // failure remains tracked for the framework's matching free() callback.
    auto &r=*free;r.owner=owner;r.lifetime=++lifetime_;r.vm=vm;r.rw=rw;r.ro=ro;r.space=space;r.thread=thread;
    r.source=static_cast<NPTManager::Owner &&>(hold);r.state=Initializing;
    for(unsigned i=0;i<2;++i) {
        if(!ops_.retain(ops_.context,descriptors[i])) {r.state=Failed;return {};}
        r.descriptors[i]=descriptors[i];
        if(!ops_.inspect(ops_.context,descriptors[i],r.buffers[i]) || !MemoryAccess::span(r.buffers[i].address,r.buffers[i].bytes)) {
            r.state=Failed;return {};
        }
    }
    // RW interfaces have a reviewed 4K stride; RO interfaces are packed. Prove
    // only the RO pointer, without inventing a per-vCPU RO size/alignment.
    if(!contains(r.buffers[0],rw,4096) || !contains(r.buffers[1],ro,1) || overlap(r.buffers[0],r.buffers[1])) {
        r.state=Failed;return {};
    }
    return Initialization(this,size_t(free-records_));
}
bool Registry::claim(Record &r,uint64_t pa) {
    if(r.pa) return r.pa==pa && r.core;
    if(r.state!=Initializing || r.thread!=ops_.thread(ops_.context)) return false;
    uintptr_t descriptor=0;uint64_t actual=0;
    if(!AppleHVABI::field(ops_.memory,r.owner,0x88,descriptor) || !AppleHVABI::field(ops_.memory,r.owner,0x90,actual) ||
       actual!=pa || !descriptor) return false;
    for(auto &other:records_) if(&other!=&r && other.state!=Empty && other.pa==pa) return false;
    // init_vmcs has already prepared this descriptor and stored the result of
    // getPhysicalSegment in +0x90 before the first VMCLEAR. Only that scoped
    // data flow authorizes a PA; a translation through the physmap does not.
    if(!r.descriptors[2]) {
        if(!ops_.retain(ops_.context,descriptor)) return false;
        r.descriptors[2]=descriptor;
    } else if(r.descriptors[2]!=descriptor) return false;
    Buffer buffer;
    if(!ops_.inspect(ops_.context,descriptor,buffer) || !buffer.address || buffer.bytes!=4096 || (buffer.address&4095) ||
       !MemoryAccess::span(buffer.address,buffer.bytes) || overlap(buffer,r.buffers[0]) || overlap(buffer,r.buffers[1])) return false;
    uint32_t revision=0;
    if(!ops_.memory.load(buffer.address,&revision,sizeof(revision)) || revision!=1) return false;
    // Existing untagged state is not ours. Never adopt a slot allocated from an
    // arbitrary PA by another path or a previous object at the same address.
    auto core=SoftVMCS::claimOwned(pa,r.owner,r.lifetime,r.space);if(!core) return false;
    r.buffers[2]=buffer;r.pa=pa;r.core=true;return true;
}
bool Registry::finish(size_t index,bool success) {
    auto &r=records_[index];Runtime::Guard guard(gate_);
    bool accepted=guard && success && ops_.thread(ops_.context)==r.thread && r.state==Initializing && r.core;
    if(accepted) {
        auto core=SoftVMCS::findByPhysAddr(r.pa);
        accepted=core && core->appleOwner==r.owner && core->appleLifetime==r.lifetime &&
            core->space.owner()==r.space.owner() && core->space.lifetime()==r.space.lifetime() && core->space.eptp()==r.space.eptp() &&
            core->ownerCPU==SoftVMCS::MAX_CPUS && !core->launched;
    }
    // A failed admission still completes initialization. No gate-dependent
    // cleanup may leave a permanently Initializing record behind.
    r.state=accepted?Live:Failed;return accepted;
}
Registry::Lease Registry::acquire(uint64_t owner) {
    Runtime::Guard guard(gate_);if(!guard || !configured_) return {};
    for(auto &r:records_) if(r.owner==owner && (r.state==Live || (r.state==Initializing && r.core && r.thread==ops_.thread(ops_.context))) &&
       __atomic_load_n(&r.references,__ATOMIC_RELAXED)!=UINT32_MAX) {
        __atomic_add_fetch(&r.references,1U,__ATOMIC_RELAXED);return Lease(this,size_t(&r-records_));
    }
    return {};
}
Registry::Lease Registry::acquirePhysical(uint64_t pa) {
    Runtime::Guard guard(gate_);if(!guard || !configured_ || !validPA(pa)) return {};
    Record *candidate=nullptr;
    for(auto &r:records_) if(r.state!=Empty && r.pa==pa) {
        if(r.state!=Live && !(r.state==Initializing && r.thread==ops_.thread(ops_.context))) return {};
        candidate=&r;break;
    }
    if(!candidate) for(auto &r:records_) if(r.state==Initializing && r.thread==ops_.thread(ops_.context)) {candidate=&r;break;}
    if(!candidate || __atomic_load_n(&candidate->references,__ATOMIC_RELAXED)==UINT32_MAX || !claim(*candidate,pa)) return {};
    __atomic_add_fetch(&candidate->references,1U,__ATOMIC_RELAXED);return Lease(this,size_t(candidate-records_));
}
void Registry::release(size_t index) {__atomic_sub_fetch(&records_[index].references,1U,__ATOMIC_RELEASE);}
Registry::Retirement Registry::retire(uint64_t owner) {
    Runtime::Guard guard(gate_);if(!guard || !configured_) return {};
    for(auto &r:records_) if(r.owner==owner && r.state!=Empty && r.state!=Retired) {
        if(r.state==Initializing || (r.state==Retiring && !r.source)) return {};
        r.state=Retiring; // refuse new metadata leases even when this try is busy
        if(__atomic_load_n(&r.references,__ATOMIC_ACQUIRE)) return {};
        if(r.core && !SoftVMCS::releaseByPhysAddr(r.pa)) return {};
        r.core=false;r.source.reset();return Retirement(this,size_t(&r-records_));
    }
    return {};
}
void Registry::completeRetirement(size_t index) {
    auto &r=records_[index];
    for(auto &descriptor:r.descriptors) if(descriptor) {ops_.release(ops_.context,descriptor);descriptor=0;}
    r.state=Retired;
}
bool Registry::untracked(uint64_t owner) {
    Runtime::Guard guard(gate_);if(!guard || !configured_) return false;
    reap();for(auto &r:records_) if(r.state!=Empty && r.owner==owner) return false;return true;
}
bool Registry::empty() {
    Runtime::Guard guard(gate_);if(!guard || !configured_) return false;
    reap();for(auto &r:records_) if(r.state!=Empty) return false;return true;
}
uint64_t Registry::Lease::owner() const {return registry_?registry_->records_[index_].owner:0;}
uint64_t Registry::Lease::lifetime() const {return registry_?registry_->records_[index_].lifetime:0;}
uint64_t Registry::Lease::physical() const {return registry_?registry_->records_[index_].pa:0;}
NPTManager::SpaceKey Registry::Lease::space() const {return registry_?registry_->records_[index_].space:NPTManager::SpaceKey{};}
bool Registry::Lease::owns(uint64_t address,size_t bytes,bool write) const {
    if(!registry_) return false;
    auto &r=registry_->records_[index_];
    if(contains({r.rw,4096},address,bytes)) return true;
    return !write && (contains({r.owner,0x394},address,bytes) || contains(r.buffers[1],address,bytes) || contains(r.buffers[2],address,bytes));
}
bool Registry::Lease::readRevision(uint64_t pa,void *out,size_t bytes) const {
    if(!registry_ || !out || bytes!=sizeof(uint32_t)) return false;
    auto &r=registry_->records_[index_];
    return pa==r.pa && r.core && registry_->ops_.memory.load(r.buffers[2].address,out,bytes);
}
}
