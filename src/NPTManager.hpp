#ifndef AMDHV_NPT_MANAGER_HPP
#define AMDHV_NPT_MANAGER_HPP
#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>
namespace NPTManager {
constexpr uint64_t PHYS_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t EPT_READ=1, EPT_WRITE=2, EPT_EXEC=4, EPT_RWX_MASK=7;
constexpr uint64_t EPT_MEM_SHIFT=3, EPT_MEM_MASK=7ULL<<3;
constexpr uint64_t EPT_LARGE_PAGE=1ULL<<7, EPT_ACCESSED=1ULL<<8, EPT_DIRTY=1ULL<<9;
constexpr uint64_t NPT_PRESENT=1, NPT_RW=2, NPT_USER=4, NPT_PWT=8, NPT_PCD=16;
constexpr uint64_t NPT_ACCESSED=32, NPT_DIRTY=64, NPT_PS=128, NPT_NX=1ULL<<63;
class SpaceKey {
    uint64_t owner_ {},lifetime_ {},eptp_ {};
    friend struct KeyFactory;
    SpaceKey(uint64_t owner,uint64_t lifetime,uint64_t eptp):owner_(owner),lifetime_(lifetime),eptp_(eptp) {}
public:
    SpaceKey()=default;
    uint64_t owner() const {return owner_;}
    uint64_t lifetime() const {return lifetime_;}
    uint64_t eptp() const {return eptp_;}
    explicit operator bool() const {return owner_ && lifetime_ && eptp_;}
};
SpaceKey registerSpace(uint64_t owner,uint64_t eptp);
SpaceKey lookupSpace(uint64_t owner,uint64_t eptp);
class Owner {
    size_t index_ {4};
    SpaceKey key_;
    friend Owner retainSpace(const SpaceKey &);
    Owner(size_t index,SpaceKey key):index_(index),key_(key) {}
public:
    Owner()=default;~Owner() {reset();}
    Owner(const Owner &)=delete;Owner &operator=(const Owner &)=delete;
    Owner(Owner &&other) noexcept:index_(other.index_),key_(other.key_) {other.index_=4;other.key_={};}
    Owner &operator=(Owner &&other) noexcept {if(this!=&other) {reset();index_=other.index_;key_=other.key_;other.index_=4;other.key_={};}return *this;}
    void reset();
    explicit operator bool() const {return index_<4;}
};
// vCPU lifetimes pin the source identity without preventing mapping updates.
Owner retainSpace(const SpaceKey &key);
bool hasNoOwners(const SpaceKey &key);

// Source ownership adapter. begin pins the source hierarchy AND prevents edits
// until end. read must reject unmapped/unowned physical spans without faulting.
// These callbacks run under nonblocking admission and must not reenter NPT.
// Trap/exit adapters must use nonblocking pinning and return failure on contention;
// they must never wait on a lock that the interrupted thread could own.
struct SourceAccess {
    void *context {};
    bool (*begin)(void *, uint64_t eptp) {};
    bool (*read)(void *, uint64_t pa, void *dst, size_t bytes) {};
    void (*end)(void *) {};
    // Prove the complete leaf HPA span belongs to this VM, before publishing it.
    bool (*allowMapping)(void *, uint64_t eptp, uint64_t gpa, uint64_t hpa, uint64_t bytes, uint8_t permissions) {};
    bool (*beginOwned)(void *,const SpaceKey &) {};
    // Must refuse while retained maps/wires or other source owners remain.
    bool (*canRetire)(void *,const SpaceKey &) {};
};
// Called only with no executing root leases. Success means every listed CPU has
// synchronously flushed ALL guest translations; false quarantines the root.
struct Shootdown {
    void *context {};
    bool (*flush)(void *, uint64_t root, uint64_t generation, const uint64_t cpus[2]) {};
};
void init(uint64_t physmapBase = 0);
bool configure(SourceAccess source, Shootdown shootdown = {});
bool prepareResources(size_t pagesPerSpace = 64);
bool resourcesReady();
bool shutdown();
class Lease {
    size_t index_ {4};
    uint64_t root_ {0}, generation_ {0};
    SpaceKey key_;
    friend Lease acquire(uint64_t,uint32_t);
    friend Lease acquire(const SpaceKey &,uint32_t);
    Lease(size_t index, uint64_t root, uint64_t generation,SpaceKey key={}) : index_(index), root_(root), generation_(generation),key_(key) {}
public:
    Lease() = default;
    ~Lease();
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept;
    Lease &operator=(Lease &&other) noexcept;
    void reset();
    explicit operator bool() const { return root_ != 0; }
    uint64_t root() const { return root_; }
    uint64_t generation() const { return generation_; }
    bool matches(const SpaceKey &key) const {return key && key_.owner()==key.owner() && key_.lifetime()==key.lifetime() && key_.eptp()==key.eptp();}
};
Lease acquire(uint64_t eptp, uint32_t cpu);
Lease acquire(const SpaceKey &key,uint32_t cpu);
class Update {
    size_t index_ {4};
    SpaceKey key_;
    explicit Update(size_t index,SpaceKey key={}):index_(index),key_(key) {}
    friend Update beginUpdate(uint64_t);
    friend Update beginUpdate(const SpaceKey &);
    friend bool retire(Update &);
public:
    Update()=default;
    ~Update();
    Update(const Update &)=delete;
    Update &operator=(const Update &)=delete;
    Update(Update &&other) noexcept;
    Update &operator=(Update &&other) noexcept;
    void reset();
    explicit operator bool() const {return index_<4;}
    bool matches(const SpaceKey &key) const {return bool(*this) && key && key_.owner()==key.owner() && key_.lifetime()==key.lifetime() && key_.eptp()==key.eptp();}
};
// Flush and revoke before changing/freeing the source EPT. The ticket blocks
// new entries until the caller completes its source operation and releases it.
Update beginUpdate(uint64_t eptp);
Update beginUpdate(const SpaceKey &key);
bool releaseSpace(const SpaceKey &key);
bool invalidateSpace(const SpaceKey &key);
bool retire(Update &update);

uint64_t convertEPTEntryToNPT(uint64_t entry, bool leaf, bool large = false);
uint64_t convertNPFInfoToEPTQualification(uint64_t info, uint64_t effectiveEntry);
enum class SyncResult { Synced, NotMapped, Unsupported, Unowned, Busy, NoResources, ShootdownFailure };
struct Mapping {uint64_t gpa {},hpa {};uint8_t permissions {};};
// Inspect under a revoked update without granting hardware access. Used to wire
// the actual guest-map page before a separate, ownership-checked shadow rebuild.
SyncResult inspectMapping(const SpaceKey &key,const Update &revoked,uint64_t gpa,Mapping &out);
SyncResult synchronize(uint64_t eptp, uint64_t gpa, uint64_t *effectiveEntry = nullptr);
SyncResult synchronize(const SpaceKey &key,uint64_t gpa,uint64_t *effectiveEntry=nullptr);
bool syncGPAFromEPT(uint64_t eptp, uint64_t gpa, uint64_t *effectiveEntry = nullptr);
bool syncGPARangeFromEPT(uint64_t eptp, uint64_t gpa, uint64_t size);
bool invalidateEPTP(uint64_t eptp);
// Trap-time INVEPT acknowledgement only: every matching root must already have
// a revoked source-update ticket. Never dispatches a shootdown or clears it.
bool acknowledgeRevoked(uint64_t eptp);
bool releaseEPTP(uint64_t eptp);
bool readGuestPhysical(uint64_t eptp, uint64_t gpa, void *dst, size_t length);
bool readGuestPhysical(const SpaceKey &key,uint64_t gpa,void *dst,size_t length);
// Offline inspection only. Execution must hold acquire() instead.
#ifndef AMDHV_KERNEL_BUILD
uint64_t getOrCreateNCR3(uint64_t eptp, uint64_t &generation);
#endif
}
#endif
