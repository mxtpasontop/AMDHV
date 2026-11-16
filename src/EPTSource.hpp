#ifndef AMDHV_EPT_SOURCE_HPP
#define AMDHV_EPT_SOURCE_HPP
#include <src/GuestMemory.hpp>
namespace EPTSource {
struct Operations {
    void *context {};
    bool (*tryLock)(void *,uintptr_t) {};
    void (*unlock)(void *,uintptr_t) {};
    bool (*root)(void *,uintptr_t,uint64_t &) {};
    bool (*readPhysical)(void *,uint64_t,void *,size_t) {};
};
// Pins the actual guest pmap while a walk is in progress. Only the root and
// validated descendants may be read as tables. Guest leaf reads/mappings require
// the separate wire registry's complete GPA/HPA ownership proof.
class Adapter {
    struct Space {NPTManager::SpaceKey key;uintptr_t pmap {};};
    Space spaces_[4];
    Runtime::Gate gate_;
    GuestMemory::Registry *memory_ {};
    Operations ops_;
    NPTManager::SpaceKey active_;
    uintptr_t activePmap_ {};
    uint64_t tables_[4] {};
    Space *find(const NPTManager::SpaceKey &key);
    bool begin(const NPTManager::SpaceKey &key);
    void end();
    bool read(uint64_t pa,void *out,size_t size);
    bool canRetire(const NPTManager::SpaceKey &key);
public:
    bool configure(Operations operations,GuestMemory::Registry &memory);
    GuestMemory::Result bind(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked,uintptr_t map,uintptr_t pmap);
    GuestMemory::Result release(const NPTManager::SpaceKey &key,const NPTManager::Update &revoked);
    NPTManager::SourceAccess sourceAccess();
};
}
#endif
