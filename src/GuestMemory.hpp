#ifndef AMDHV_GUEST_MEMORY_HPP
#define AMDHV_GUEST_MEMORY_HPP
#include <src/NPTManager.hpp>
#include <src/Runtime.hpp>
namespace GuestMemory {
// These operations run only in sleepable source-map setup/teardown contexts.
// wire is transactional: nonzero return must leave the page unwired.
struct Operations {
    void *context {};
    void (*retainMap)(void *,uintptr_t) {};
    void (*releaseMap)(void *,uintptr_t) {};
    int (*wire)(void *,uintptr_t,uint64_t,uint32_t,uint64_t &) {};
    int (*unwire)(void *,uintptr_t,uint64_t,uint64_t) {};
    bool (*readPhysical)(void *,uint64_t,void *,size_t) {};
};
enum class Result { Success, Busy, Unowned, InvalidRange, NoResources, WireFailure, UnwireFailure };
class Registry {
    static constexpr size_t Spaces=4,Runs=1024;
    struct Run {uint64_t gpa {},hpa {},size {};uint8_t permissions {};};
    struct Space {NPTManager::SpaceKey key;uintptr_t map {};size_t count {};bool faulted {};Run runs[Runs+1];};
    Space spaces_[Spaces];
    Runtime::Gate gate_;
    Operations ops_;
    bool configured_ {};
    Space *find(const NPTManager::SpaceKey &key);
    Result unwind(Space &space,size_t start,uint64_t oldLastSize);
public:
    bool configure(Operations operations);
    Result adopt(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uintptr_t map);
    Result wireRange(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uint64_t gpa,uint64_t size,uint8_t permissions);
    Result unwireRange(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uint64_t gpa,uint64_t size);
    Result release(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked);
    bool owns(const NPTManager::SpaceKey &key,uint64_t gpa,uint64_t hpa,uint64_t size,uint8_t permissions);
    bool read(const NPTManager::SpaceKey &key,uint64_t hpa,void *out,size_t size);
    // NPT calls this before retiring a key. Contention refuses retirement.
    bool canRetire(const NPTManager::SpaceKey &key);
    bool empty();
};
}
#endif
