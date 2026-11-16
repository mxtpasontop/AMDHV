#ifndef AMDHV_VCPU_LIFECYCLE_HPP
#define AMDHV_VCPU_LIFECYCLE_HPP
#include <src/SoftVMCS.hpp>
#include <src/Runtime.hpp>
namespace VcpuLifecycle {
struct Buffer {uint64_t address {};size_t bytes {};};
struct Operations {
    MemoryAccess::Access memory;
    void *context {};
    uintptr_t (*thread)(void *) {};
    bool (*retain)(void *,uintptr_t descriptor) {};
    void (*release)(void *,uintptr_t descriptor) {};
    bool (*inspect)(void *,uintptr_t descriptor,Buffer &) {};
    // Read/retain/inspect/thread callbacks are nonblocking and cannot reenter
    // this registry. release runs outside admission and may destroy the buffer.
    // The registry must outlive all scopes and every tracked framework object.
    // original framework caller owns the object/descriptor while retain runs.
    // inspect is a wired-buffer getter, never a physical mapping lookup.
};
class Registry {
    static constexpr size_t Capacity=SoftVMCS::MAX_VMCS_SLOTS;
    enum State : uint8_t {Empty,Initializing,Live,Failed,Retiring,Retired};
    class AtomicState {
        uint8_t value_ {};
    public:
        operator State() const {return State(__atomic_load_n(&value_,__ATOMIC_ACQUIRE));}
        AtomicState &operator=(State value) {__atomic_store_n(&value_,uint8_t(value),__ATOMIC_RELEASE);return *this;}
    };
    struct Record {
        uint64_t owner {},lifetime {},vm {},rw {},ro {},pa {};
        uintptr_t thread {},descriptors[3] {};
        Buffer buffers[3]; // parent RW, parent RO, vCPU VMCS
        NPTManager::SpaceKey space;
        NPTManager::Owner source;
        uint32_t references {};
        AtomicState state;
        bool core {};
    } records_[Capacity];
    Operations ops_;
    Runtime::Gate gate_;
    bool configured_ {};
    uint64_t lifetime_ {};
    void reap();
    bool finish(size_t,bool);
    void release(size_t);
    void completeRetirement(size_t);
    bool claim(Record &,uint64_t);
public:
    class Lease {
        Registry *registry_ {};size_t index_ {};
        Lease(Registry *registry,size_t index):registry_(registry),index_(index) {}
        friend class Registry;
    public:
        Lease()=default;~Lease() {reset();}
        Lease(const Lease &)=delete;Lease &operator=(const Lease &)=delete;
        Lease(Lease &&other) noexcept:registry_(other.registry_),index_(other.index_) {other.registry_=nullptr;}
        Lease &operator=(Lease &&other) noexcept {if(this!=&other) {reset();registry_=other.registry_;index_=other.index_;other.registry_=nullptr;}return *this;}
        void reset() {if(registry_) registry_->release(index_);registry_=nullptr;}
        explicit operator bool() const {return registry_!=nullptr;}
        uint64_t owner() const;
        uint64_t lifetime() const;
        uint64_t physical() const;
        NPTManager::SpaceKey space() const;
        bool owns(uint64_t address,size_t bytes,bool write=false) const;
        // Only the revision header is exposed to the VMX emulator. This reads
        // the retained buffer VA; it never reads arbitrary physical memory.
        bool readRevision(uint64_t physical,void *out,size_t bytes) const;
    };
    class Initialization {
        Registry *registry_ {};size_t index_ {};
        Initialization(Registry *registry,size_t index):registry_(registry),index_(index) {}
        friend class Registry;
    public:
        Initialization()=default;~Initialization() {finish(false);}
        Initialization(const Initialization &)=delete;Initialization &operator=(const Initialization &)=delete;
        Initialization(Initialization &&other) noexcept:registry_(other.registry_),index_(other.index_) {other.registry_=nullptr;}
        explicit operator bool() const {return registry_!=nullptr;}
        bool finish(bool success) {if(!registry_) return false;auto r=registry_;registry_=nullptr;return r->finish(index_,success);}
    };
    class Retirement {
        Registry *registry_ {};size_t index_ {};
        Retirement(Registry *registry,size_t index):registry_(registry),index_(index) {}
        friend class Registry;
    public:
        Retirement()=default;~Retirement() {reset();}
        Retirement(const Retirement &)=delete;Retirement &operator=(const Retirement &)=delete;
        Retirement(Retirement &&other) noexcept:registry_(other.registry_),index_(other.index_) {other.registry_=nullptr;}
        Retirement &operator=(Retirement &&other) noexcept {if(this!=&other) {reset();registry_=other.registry_;index_=other.index_;other.registry_=nullptr;}return *this;}
        explicit operator bool() const {return registry_!=nullptr;}
        // Hold through the original free(). It may destroy the vCPU; reset
        // uses only our retained descriptors and never touches that object.
        void reset() {if(registry_) registry_->completeRetirement(index_);registry_=nullptr;}
    };
    bool configure(Operations operations);
    bool ready();
    Initialization begin(uint64_t owner,const NPTManager::SpaceKey &space);
    Lease acquire(uint64_t owner);
    Lease acquirePhysical(uint64_t physical);
    Retirement retire(uint64_t owner);
    bool untracked(uint64_t owner);
    bool empty();
};
Registry &global();
}
#endif
