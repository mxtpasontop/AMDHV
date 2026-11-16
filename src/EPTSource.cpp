#include <src/EPTSource.hpp>
#include <Headers/Lilu/kern_util.hpp>
namespace EPTSource {
Adapter::Space *Adapter::find(const NPTManager::SpaceKey &key) {
    if(!key) return nullptr;
    for(auto &s:spaces_) if(s.key.owner()==key.owner() && s.key.lifetime()==key.lifetime() && s.key.eptp()==key.eptp()) return &s;
    return nullptr;
}
bool Adapter::configure(Operations ops,GuestMemory::Registry &memory) {
    Runtime::Guard guard(gate_);
    if(!guard || memory_ || !ops.tryLock || !ops.unlock || !ops.root || !ops.readPhysical) return false;
    ops_=ops;memory_=&memory;return true;
}
GuestMemory::Result Adapter::bind(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uintptr_t map,uintptr_t pmap) {
    using R=GuestMemory::Result;
    Runtime::Guard guard(gate_);if(!guard) return R::Busy;
    if(!memory_ || !revoked.matches(key) || !map || !pmap || find(key)) return R::Unowned;
    for(auto &s:spaces_) if(s.key && (s.key.owner()==key.owner() || s.pmap==pmap)) return R::Unowned;
    for(auto &s:spaces_) if(!s.key) {
        auto result=memory_->adopt(key,revoked,map);if(result!=R::Success) return result;
        s={key,pmap};return R::Success;
    }
    return R::NoResources;
}
GuestMemory::Result Adapter::release(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked) {
    using R=GuestMemory::Result;
    Runtime::Guard guard(gate_);if(!guard) return R::Busy;
    auto s=memory_ && revoked.matches(key)?find(key):nullptr;if(!s) return R::Unowned;
    auto result=memory_->release(key,revoked);if(result!=R::Success) return result;
    *s={};return R::Success;
}
bool Adapter::begin(const NPTManager::SpaceKey &key) {
    if(!gate_.enter()) return false;
    auto s=memory_?find(key):nullptr;
    if(!s || !ops_.tryLock(ops_.context,s->pmap)) {gate_.leave();return false;}
    uint64_t root=0;
    if(!ops_.root(ops_.context,s->pmap,root) || root!=key.eptp()) {
        ops_.unlock(ops_.context,s->pmap);gate_.leave();return false;
    }
    active_=key;activePmap_=s->pmap;
    memset(tables_,0,sizeof(tables_));tables_[0]=root&NPTManager::PHYS_ADDR_MASK;
    return true;
}
void Adapter::end() {
    if(!active_) return;
    ops_.unlock(ops_.context,activePmap_);active_={};activePmap_=0;
    memset(tables_,0,sizeof(tables_));gate_.leave();
}
bool Adapter::read(uint64_t pa,void *out,size_t size) {
    if(!active_ || !out || !size || size>4096 || pa>UINT64_MAX-(size-1)) return false;
    for(unsigned depth=0;depth<4;++depth) if(tables_[depth] && (pa&~4095ULL)==tables_[depth]) {
        if(size!=8 || (pa&7)) return false;
        uint64_t entry=0;
        if(!ops_.readPhysical(ops_.context,pa,&entry,8)) return false;
        // Revoke descendant authorizations before interpreting this parent.
        for(unsigned next=depth+1;next<4;++next) tables_[next]=0;
        bool large=(entry&NPTManager::EPT_LARGE_PAGE)!=0;
        bool leaf=depth==3 || large;
        if(depth<3 && !large && NPTManager::convertEPTEntryToNPT(entry,false)) {
            uint64_t child=entry&NPTManager::PHYS_ADDR_MASK;
            // Cycles and self references are never legitimate EPT ownership.
            for(unsigned parent=0;parent<=depth;++parent) if(tables_[parent]==child) return false;
            tables_[depth+1]=child;
        } else if(!leaf && (entry&7)) return false;
        memcpy(out,&entry,8);return true;
    }
    return memory_->read(active_,pa,out,size);
}
bool Adapter::canRetire(const NPTManager::SpaceKey &key) {
    Runtime::Guard guard(gate_);return guard && !find(key) && memory_ && memory_->canRetire(key);
}
NPTManager::SourceAccess Adapter::sourceAccess() {
    return {this,
        [](void *,uint64_t) {return false;},
        [](void *p,uint64_t pa,void *out,size_t size) {return static_cast<Adapter *>(p)->read(pa,out,size);},
        [](void *p) {static_cast<Adapter *>(p)->end();},
        [](void *p,uint64_t eptp,uint64_t gpa,uint64_t hpa,uint64_t bytes,uint8_t permissions) {
            auto &s=*static_cast<Adapter *>(p);
            return s.active_ && s.active_.eptp()==eptp && s.memory_->owns(s.active_,gpa,hpa,bytes,permissions);
        },
        [](void *p,const NPTManager::SpaceKey &key) {return static_cast<Adapter *>(p)->begin(key);},
        [](void *p,const NPTManager::SpaceKey &key) {return static_cast<Adapter *>(p)->canRetire(key);}};
}
}
