#include <src/TrapRecovery.hpp>
#include <src/MemoryAccess.hpp>
namespace TrapRecovery {
static Registry global;
Registry &registry() {return global;}
Registry::Scope Registry::arm(uint32_t cpu,Record &record) {
    if(cpu>=MaxCPUs || !record.owner || !record.sites || !record.count || record.count>16) return {};
    // A recursive attempt must not reset the active record's one-fault limit.
    if(__atomic_load_n(&active_[cpu],__ATOMIC_ACQUIRE)) return {};
    for(size_t i=0;i<record.count;++i) {
        const auto &s=record.sites[i];
        constexpr uint32_t allowed=(1U<<6)|(1U<<13)|(1U<<14);
        if(!s.fault || !s.recover || !MemoryAccess::canonical(s.fault) || !MemoryAccess::canonical(s.recover) ||
           !s.traps || (s.traps&~allowed)) return {};
        for(size_t j=0;j<i;++j) if(record.sites[j].fault==s.fault) return {};
    }
    Record *expected=nullptr;
    __atomic_store_n(&record.faults,0U,__ATOMIC_RELAXED);
    if(!__atomic_compare_exchange_n(&active_[cpu],&expected,&record,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) return {};
    return Scope(this,cpu);
}
void Registry::Scope::reset() {
    if(registry_) __atomic_store_n(&registry_->active_[cpu_],static_cast<Record *>(nullptr),__ATOMIC_RELEASE);
    registry_=nullptr;
}
bool Registry::active(uint32_t cpu) const {
    return cpu<MaxCPUs && __atomic_load_n(&active_[cpu],__ATOMIC_ACQUIRE);
}
bool Registry::handle(uint32_t cpu,uintptr_t owner,XNUSavedState64 *state) {
    if(cpu>=MaxCPUs || !owner || !state || state->flavor!=15 || (state->cs&3) || state->trapno>=32) return false;
    auto record=__atomic_load_n(&active_[cpu],__ATOMIC_ACQUIRE);
    if(!record || record->owner!=owner || __atomic_load_n(&record->faults,__ATOMIC_RELAXED)) return false;
    for(size_t i=0;i<record->count;++i) {
        const auto &site=record->sites[i];
        if(site.fault==state->rip && (site.traps&(1U<<state->trapno))) {
            __atomic_store_n(&record->faults,1U,__ATOMIC_RELAXED);state->rip=site.recover;return true;
        }
    }
    return false;
}
}
