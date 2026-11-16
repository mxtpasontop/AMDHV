#include <src/GuestMemory.hpp>
#include <Headers/Lilu/kern_util.hpp>
namespace GuestMemory {
Registry::Space *Registry::find(const NPTManager::SpaceKey &key) {
    if(!key) return nullptr;
    for(auto &s:spaces_) if(s.key.owner()==key.owner() && s.key.lifetime()==key.lifetime() && s.key.eptp()==key.eptp()) return &s;
    return nullptr;
}
bool Registry::configure(Operations operations) {
    Runtime::Guard guard(gate_);
    if(!guard || configured_ || !operations.retainMap || !operations.releaseMap || !operations.wire || !operations.unwire || !operations.readPhysical) return false;
    ops_=operations;configured_=true;return true;
}
Result Registry::adopt(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uintptr_t map) {
    Runtime::Guard guard(gate_);
    if(!guard) return Result::Busy;
    if(!configured_ || !revoked.matches(key) || !map || find(key)) return Result::Unowned;
    for(auto &s:spaces_) if(s.key && (s.key.owner()==key.owner() || s.map==map)) return Result::Unowned;
    for(auto &s:spaces_) if(!s.key) {ops_.retainMap(ops_.context,map);s.key=key;s.map=map;s.count=0;s.faulted=false;return Result::Success;}
    return Result::NoResources;
}
Result Registry::unwind(Space &s,size_t start,uint64_t oldLastSize) {
    // Preserve each record until unwire succeeds; retries never double-unwire.
    while(s.count>start) {
        auto &r=s.runs[s.count-1];
        if(ops_.unwire(ops_.context,s.map,r.gpa,r.gpa+r.size)) return Result::UnwireFailure;
        r={};--s.count;
    }
    if(start && s.runs[start-1].size>oldLastSize) {
        auto &r=s.runs[start-1];
        if(ops_.unwire(ops_.context,s.map,r.gpa+oldLastSize,r.gpa+r.size)) return Result::UnwireFailure;
        r.size=oldLastSize;
    }
    return Result::Success;
}
Result Registry::wireRange(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uint64_t gpa,uint64_t size,uint8_t permissions) {
    if(!size || (gpa&4095) || (size&4095) || gpa>=(1ULL<<48) || size>(1ULL<<48)-gpa || !permissions || (permissions&~7)) return Result::InvalidRange;
    Runtime::Guard guard(gate_);
    if(!guard) return Result::Busy;
    auto s=configured_ && revoked.matches(key)?find(key):nullptr;
    if(!s) return Result::Unowned;
    if(s->faulted) return Result::UnwireFailure;
    uint64_t end=gpa+size;
    for(size_t i=0;i<s->count;++i) if(gpa<s->runs[i].gpa+s->runs[i].size && end>s->runs[i].gpa) return Result::InvalidRange;
    size_t start=s->count;uint64_t oldLastSize=start?s->runs[start-1].size:0;
    Result error=Result::Success;
    for(uint64_t address=gpa;address<end;address+=4096) {
        if(s->count>=Runs) {error=Result::NoResources;break;}
        uint64_t pa=0;
        if(ops_.wire(ops_.context,s->map,address,permissions,pa)) {error=Result::WireFailure;break;}
        // Even an invalid returned PA represents a successful wire and must be
        // tracked before validation, so rollback can pair that wire exactly.
        if(s->count && s->runs[s->count-1].gpa+s->runs[s->count-1].size==address &&
           s->runs[s->count-1].hpa+s->runs[s->count-1].size==pa && s->runs[s->count-1].permissions==permissions)
            s->runs[s->count-1].size+=4096;
        else s->runs[s->count++]={address,pa,4096,permissions};
        if(!pa || (pa&~NPTManager::PHYS_ADDR_MASK)) {error=Result::WireFailure;break;}
    }
    if(error==Result::Success) return error;
    auto rollback=unwind(*s,start,oldLastSize);
    if(rollback!=Result::Success) s->faulted=true;
    return rollback==Result::Success?error:rollback;
}
Result Registry::release(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked) {
    Runtime::Guard guard(gate_);
    if(!guard) return Result::Busy;
    auto s=configured_ && revoked.matches(key)?find(key):nullptr;
    if(!s) return Result::Unowned;
    auto result=unwind(*s,0,0);if(result!=Result::Success) {s->faulted=true;return result;}
    auto map=s->map;s->map=0;s->key={};s->faulted=false;ops_.releaseMap(ops_.context,map);return Result::Success;
}
Result Registry::unwireRange(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uint64_t gpa,uint64_t size) {
    if(!size || (gpa&4095) || (size&4095) || gpa>=(1ULL<<48) || size>(1ULL<<48)-gpa) return Result::InvalidRange;
    Runtime::Guard guard(gate_);if(!guard) return Result::Busy;
    auto s=configured_ && revoked.matches(key)?find(key):nullptr;
    if(!s) return Result::Unowned;
    if(s->faulted) return Result::UnwireFailure;
    uint64_t end=gpa+size;
    // At most one run can contain both boundaries. Reserve its split before
    // the first unwire so capacity failure cannot leave a partial operation.
    if(s->count>=Runs) for(size_t i=0;i<s->count;++i) {
        const auto &r=s->runs[i];
        if(gpa>r.gpa && end<r.gpa+r.size) return Result::NoResources;
    }
    for(size_t i=0;i<s->count;) {
        auto &r=s->runs[i];uint64_t last=r.gpa+r.size;
        if(gpa>=last || end<=r.gpa) {++i;continue;}
        uint64_t first=gpa>r.gpa?gpa:r.gpa,stop=end<last?end:last;
        if(ops_.unwire(ops_.context,s->map,first,stop)) {s->faulted=true;return Result::UnwireFailure;}
        // Commit each successful unwire immediately. Later failure must never
        // cause release/retry to unwire this span a second time.
        if(first==r.gpa && stop==last) {r=s->runs[--s->count];s->runs[s->count]={};continue;}
        if(first==r.gpa) {r.hpa+=stop-r.gpa;r.size=last-stop;r.gpa=stop;}
        else {
            if(stop<last) s->runs[s->count++]={stop,r.hpa+(stop-r.gpa),last-stop,r.permissions};
            r.size=first-r.gpa;
        }
        ++i;
    }
    return Result::Success;
}
bool Registry::owns(const NPTManager::SpaceKey &key,uint64_t gpa,uint64_t hpa,uint64_t size,uint8_t permissions) {
    if(!size || (gpa&4095) || (hpa&4095) || (size&4095) || gpa>=(1ULL<<48) || size>(1ULL<<48)-gpa ||
       hpa>UINT64_MAX-size || !permissions || (permissions&~7)) return false;
    Runtime::Guard guard(gate_);auto s=guard?find(key):nullptr;if(!s || s->faulted) return false;
    while(size) {
        const Run *found=nullptr;
        for(size_t i=0;i<s->count;++i) {
            auto &r=s->runs[i];
            if(gpa>=r.gpa && gpa-r.gpa<r.size && hpa>=r.hpa && hpa-r.hpa==gpa-r.gpa && !(permissions&~r.permissions)) {found=&r;break;}
        }
        if(!found) return false;
        uint64_t n=found->size-(gpa-found->gpa);if(n>size) n=size;
        size-=n;gpa+=n;hpa+=n;
    }
    return true;
}
bool Registry::read(const NPTManager::SpaceKey &key,uint64_t hpa,void *out,size_t size) {
    if(!out || !size || size>4096 || hpa>UINT64_MAX-size) return false;
    Runtime::Guard guard(gate_);auto s=guard?find(key):nullptr;if(!s || s->faulted) return false;
    for(size_t i=0;i<s->count;++i) {
        auto &r=s->runs[i];
        if(hpa>=r.hpa && hpa-r.hpa<=r.size && size<=r.size-(hpa-r.hpa) && (r.permissions&1))
            return ops_.readPhysical(ops_.context,hpa,out,size);
    }
    return false;
}
bool Registry::empty() {
    Runtime::Guard guard(gate_);if(!guard) return false;
    for(auto &s:spaces_) if(s.key) return false;
    return true;
}
bool Registry::canRetire(const NPTManager::SpaceKey &key) {
    Runtime::Guard guard(gate_);return guard && !find(key);
}
}
