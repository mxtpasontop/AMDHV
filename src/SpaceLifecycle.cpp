#include <src/SpaceLifecycle.hpp>
namespace SpaceLifecycle {
static Context instance;
Context &global() {return instance;}
Context::Space *Context::find(uint64_t owner) {
    for(auto &s:spaces_) if(s.key.owner()==owner) return &s;
    return nullptr;
}
void Context::reap() {
    for(auto &s:spaces_) if(s.retired && !__atomic_load_n(&s.depth,__ATOMIC_ACQUIRE)) {
        s.key={};s.map=s.pmap=s.thread=0;s.bound=s.retired=false;
    }
}
bool Context::configure(GuestMemory::Operations memory,EPTSource::Operations source,NPTManager::Shootdown shootdown,
                        uintptr_t (*thread)(void *),void *threadContext) {
    Runtime::Guard guard(gate_);
    if(!guard || configured_ || failed_ || !thread) return false;
    if(!memory_.configure(memory) || !source_.configure(source,memory_) || !NPTManager::configure(source_.sourceAccess(),shootdown)) {
        failed_=true;return false;
    }
    thread_=thread;threadContext_=threadContext;configured_=true;return true;
}
Result Context::attach(const AppleHVABI::Space &snapshot) {
    Runtime::Guard guard(gate_);if(!guard) return Result::Busy;
    if(!configured_ || failed_ || !snapshot.address || !snapshot.map || !snapshot.pmap) return Result::Unowned;
    reap();if(find(snapshot.address)) return Result::Unowned;
    Space *record=nullptr;for(auto &s:spaces_) if(!s.key) {record=&s;break;}
    if(!record) return Result::NoResources;
    auto key=NPTManager::registerSpace(snapshot.address,snapshot.eptp);if(!key) return Result::NoResources;
    // Retain cleanup identity before any fallible binding. Failed init will run
    // free(); even a contended rollback must not orphan the registered NPT key.
    record->key=key;record->map=snapshot.map;record->pmap=snapshot.pmap;
    auto update=NPTManager::beginUpdate(key);
    auto result=update?source_.bind(key,update,snapshot.map,snapshot.pmap):Result::Busy;
    if(result==Result::Success) {record->bound=true;return result;}
    if(update && NPTManager::retire(update)) {record->retired=true;}
    return result;
}
Context::Mutation Context::begin(const AppleHVABI::Space &snapshot) {
    Runtime::Guard guard(gate_);if(!guard || !configured_) return {};
    auto s=find(snapshot.address);uintptr_t thread=thread_(threadContext_);
    if(!s || s->retired || !thread || s->key.eptp()!=snapshot.eptp || s->map!=snapshot.map || s->pmap!=snapshot.pmap) return {};
    uint32_t depth=__atomic_load_n(&s->depth,__ATOMIC_ACQUIRE);
    if(depth) {
        if(s->thread!=thread || depth==UINT32_MAX) return {};
        __atomic_add_fetch(&s->depth,1U,__ATOMIC_RELAXED);
    } else {
        s->update=NPTManager::beginUpdate(s->key);if(!s->update) return {};
        s->thread=thread;__atomic_store_n(&s->depth,1U,__ATOMIC_RELEASE);
    }
    return Mutation(this,size_t(s-spaces_),thread);
}
void Context::end(size_t index,uintptr_t thread) {
    auto &s=spaces_[index];
    if(thread_(threadContext_)!=thread) {failed_=true;return;}
    // No manager gate is needed to end. A contending hook must not leak its
    // predecessor's ticket. Other threads see depth>0 until reset is complete.
    if(__atomic_load_n(&s.depth,__ATOMIC_ACQUIRE)>1) __atomic_sub_fetch(&s.depth,1U,__ATOMIC_RELEASE);
    else {s.update.reset();__atomic_store_n(&s.depth,0U,__ATOMIC_RELEASE);}
}
Result Context::Mutation::unwire(uint64_t gpa,uint64_t size) {
    if(!context_) return Result::Unowned;
    Runtime::Guard guard(context_->gate_);if(!guard) return Result::Busy;
    auto &s=context_->spaces_[index_];
    if(context_->thread_(context_->threadContext_)!=thread_ || !s.bound || s.retired) return Result::Unowned;
    auto result=context_->memory_.unwireRange(s.key,s.update,gpa,size);
    if(result==Result::UnwireFailure) context_->failed_=true;
    return result;
}
Result Context::Mutation::pinFaultPage(uint64_t gpa) {
    if(!context_) return Result::Unowned;
    Runtime::Guard guard(context_->gate_);if(!guard) return Result::Busy;
    auto &s=context_->spaces_[index_];
    if(context_->failed_ || context_->thread_(context_->threadContext_)!=thread_ || !s.bound || s.retired) return Result::Unowned;
    NPTManager::Mapping mapping;
    auto inspected=NPTManager::inspectMapping(s.key,s.update,gpa,mapping);
    if(inspected!=NPTManager::SyncResult::Synced) return inspected==NPTManager::SyncResult::Busy?Result::Busy:Result::Unowned;
    // Caller unwires this page before the source fault. Rewire after that fault,
    // then the normal NPT path independently rechecks its resulting HPA/protection.
    return context_->memory_.wireRange(s.key,s.update,mapping.gpa,4096,mapping.permissions);
}
Result Context::Mutation::retire() {
    if(!context_) return Result::Unowned;
    Runtime::Guard guard(context_->gate_);if(!guard) return Result::Busy;
    auto &s=context_->spaces_[index_];
    if(context_->thread_(context_->threadContext_)!=thread_ || s.retired || __atomic_load_n(&s.depth,__ATOMIC_ACQUIRE)!=1) return Result::Unowned;
    if(!NPTManager::hasNoOwners(s.key)) return Result::Busy;
    if(s.bound) {
        auto result=context_->source_.release(s.key,s.update);
        if(result!=Result::Success) {if(result==Result::UnwireFailure) context_->failed_=true;return result;}
        s.bound=false;
    }
    if(!NPTManager::retire(s.update)) return Result::Busy;
    s.retired=true;return Result::Success;
}
NPTManager::SpaceKey Context::lookup(const AppleHVABI::Space &snapshot) {
    Runtime::Guard guard(gate_);if(!guard || failed_) return {};
    auto s=find(snapshot.address);
    return s && s->bound && !s->retired && s->map==snapshot.map && s->pmap==snapshot.pmap && s->key.eptp()==snapshot.eptp?s->key:NPTManager::SpaceKey{};
}
bool Context::untracked(uint64_t owner) {
    Runtime::Guard guard(gate_);if(!guard) return false;
    reap();return !find(owner);
}
bool Context::empty() {
    Runtime::Guard guard(gate_);if(!guard) return false;
    reap();for(auto &s:spaces_) if(s.key) return false;
    return memory_.empty();
}
bool Context::ready() {
    Runtime::Guard guard(gate_);return guard && configured_ && !failed_ && NPTManager::resourcesReady();
}
}
