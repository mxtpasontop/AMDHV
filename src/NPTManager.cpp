#include <src/NPTManager.hpp>
#include <src/Memory.hpp>
#include <src/Runtime.hpp>
#include <Headers/Lilu/kern_util.hpp>
namespace NPTManager {
constexpr size_t Spaces=4, Pages=512;
struct Page { uint64_t *va {}; uint64_t pa {}, source {}; uint8_t level {}; };
struct Space {
    bool live {}, quarantined {};
    SpaceKey key;
    uint64_t eptp {}, generation {}, cpus[2] {};
    uint32_t references {},owners {};
    uint8_t updating {};
    Page root {}, pages[Pages];
};
static Space spaces[Spaces];
static Runtime::Gate gate;
static SourceAccess source;
static Shootdown shootdown;
static size_t capacity=0;
static uint64_t clock=0,lifetime=0;
static bool prepared=false;
static bool validEPTP(uint64_t eptp) {
    // WB, four levels, no A/D or other optional EPTP features.
    return (eptp & PHYS_ADDR_MASK) && (eptp & ~PHYS_ADDR_MASK) == 0x1e;
}
static void clearSpace(Space &s) {
    memset(s.root.va, 0, 4096);
    for (size_t i=0;i<capacity;++i) { s.pages[i].source=0; s.pages[i].level=0; }
    s.generation=++clock;
}
static bool flush(Space &s) {
    if (__atomic_load_n(&s.references,__ATOMIC_ACQUIRE) || __atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) return false;
    if ((s.cpus[0] || s.cpus[1]) && (!shootdown.flush ||
        !shootdown.flush(shootdown.context,s.root.pa,s.generation,s.cpus))) {
        s.quarantined=true; return false;
    }
    s.cpus[0]=s.cpus[1]=0; s.quarantined=false;
    return true;
}
static void freeResources() {
    for (auto &s:spaces) {
        Memory::release(s.root.va,4096);
        for (auto &p:s.pages) Memory::release(p.va,4096);
        // Aggregate assignment materializes a 16 KiB temporary on Clang.
        // All owned pages have been released; reset this trivial record in place.
        static_assert(__is_trivially_copyable(Space),"in-place reset requires a trivial record");
        memset(&s,0,sizeof(s));
    }
    prepared=false; capacity=0;
}
bool prepareResources(size_t count) {
    Runtime::Guard guard(gate);
    if (!guard || prepared || !count || count>Pages) return false;
    capacity=count;
    for (auto &s:spaces) {
        s.root.va=static_cast<uint64_t *>(Memory::allocate(4096,s.root.pa));
        if (!s.root.va) { freeResources(); return false; }
        for (size_t i=0;i<count;++i) {
            auto &p=s.pages[i];
            p.va=static_cast<uint64_t *>(Memory::allocate(4096,p.pa));
            if (!p.va) { freeResources(); return false; }
        }
    }
    prepared=true; return true;
}
bool resourcesReady() {
    Runtime::Guard guard(gate);
    return guard && prepared && source.begin && source.beginOwned && source.read && source.end && source.allowMapping && source.canRetire;
}
void init(uint64_t physmapBase) {
    // A physmap offset is not proof that a physical page is owned/readable.
    (void)physmapBase;
#ifndef AMDHV_KERNEL_BUILD
    prepareResources();
#endif
}
bool configure(SourceAccess access, Shootdown flushAdapter) {
    Runtime::Guard guard(gate);
    if (!guard || !access.begin || !access.read || !access.end || !access.allowMapping) return false;
    for (auto &s:spaces) if (s.live) return false;
    source=access; shootdown=flushAdapter; return true;
}
bool shutdown() {
    Runtime::Guard guard(gate);
    if (!guard) return false;
    for (auto &s:spaces) if (__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s.owners,__ATOMIC_ACQUIRE) ||
        (s.live && ((s.key && (!source.canRetire || !source.canRetire(source.context,s.key))) || !flush(s)))) return false;
    freeResources(); source={}; shootdown={}; return true;
}
static Space *find(uint64_t eptp, bool create) {
    if (!prepared || !validEPTP(eptp)) return nullptr;
    for (auto &s:spaces) if (s.live && s.eptp==eptp) return s.key?nullptr:&s;
#if defined(AMDHV_KERNEL_BUILD)
    (void)create;return nullptr; // Kernel users require an object lifetime key.
#endif
    if (create) for (auto &s:spaces) if (!s.live && !__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) {
        clearSpace(s); s.key={};s.eptp=eptp; s.live=true; s.quarantined=false; return &s;
    }
    return nullptr;
}
struct KeyFactory {
    static SpaceKey make(uint64_t owner,uint64_t epoch,uint64_t eptp) {return SpaceKey(owner,epoch,eptp);}
};
static Space *findOwned(const SpaceKey &key) {
    if(!key || !prepared) return nullptr;
    for(auto &s:spaces) if(s.live && s.key.owner()==key.owner() && s.key.lifetime()==key.lifetime() && s.eptp==key.eptp()) return &s;
    return nullptr;
}
Owner retainSpace(const SpaceKey &key) {
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    if(!s || s->quarantined || __atomic_load_n(&s->updating,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&s->owners,__ATOMIC_RELAXED)==UINT32_MAX) return {};
    __atomic_add_fetch(&s->owners,1U,__ATOMIC_RELAXED);return Owner(size_t(s-spaces),s->key);
}
void Owner::reset() {
    if(index_<Spaces) __atomic_sub_fetch(&spaces[index_].owners,1U,__ATOMIC_RELEASE);
    index_=Spaces;key_={};
}
bool hasNoOwners(const SpaceKey &key) {
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    return s && !__atomic_load_n(&s->owners,__ATOMIC_ACQUIRE);
}
SpaceKey registerSpace(uint64_t owner,uint64_t eptp) {
    Runtime::Guard guard(gate);
    if(!guard || !prepared || !owner || !validEPTP(eptp) || !source.beginOwned || !source.canRetire || lifetime==UINT64_MAX) return {};
    for(auto &s:spaces) if(s.live && (s.eptp==eptp || s.key.owner()==owner)) return {};
    for(auto &s:spaces) if(!s.live && !__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) {
        clearSpace(s);s.eptp=eptp;s.key=KeyFactory::make(owner,++lifetime,eptp);
        s.live=true;s.quarantined=false;return s.key;
    }
    return {};
}
SpaceKey lookupSpace(uint64_t owner,uint64_t eptp) {
    Runtime::Guard guard(gate);
    if(guard) for(auto &s:spaces) if(s.live && s.key.owner()==owner && s.eptp==eptp) return s.key;
    return {};
}
Lease acquire(const SpaceKey &key,uint32_t cpu) {
    Runtime::Guard guard(gate);
    auto s=guard && cpu<128?findOwned(key):nullptr;
    if(!s || s->quarantined || __atomic_load_n(&s->updating,__ATOMIC_ACQUIRE)) return {};
    __atomic_add_fetch(&s->references,1U,__ATOMIC_RELAXED);
    s->cpus[cpu/64]|=1ULL<<(cpu%64);
    return Lease(size_t(s-spaces),s->root.pa,s->generation,s->key);
}
bool invalidateSpace(const SpaceKey &key) {
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    if(!s || !flush(*s)) return false;
    clearSpace(*s);return true;
}
bool releaseSpace(const SpaceKey &key) {
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    if(!s || __atomic_load_n(&s->owners,__ATOMIC_ACQUIRE) || !source.canRetire(source.context,key) || !flush(*s)) return false;
    clearSpace(*s);s->live=false;s->key={};s->eptp=0;return true;
}
Update beginUpdate(const SpaceKey &key) {
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    if(!s || !flush(*s)) return {};
    clearSpace(*s);__atomic_store_n(&s->updating,uint8_t(1),__ATOMIC_RELEASE);
    return Update(size_t(s-spaces),s->key);
}
Lease::~Lease() { reset(); }
Lease::Lease(Lease &&other) noexcept : index_(other.index_),root_(other.root_),generation_(other.generation_),key_(other.key_) { other.root_=0; }
Lease &Lease::operator=(Lease &&other) noexcept {
    if (this!=&other) { reset(); index_=other.index_;root_=other.root_;generation_=other.generation_;key_=other.key_;other.root_=0; }
    return *this;
}
void Lease::reset() {
    if (root_) __atomic_sub_fetch(&spaces[index_].references,1U,__ATOMIC_RELEASE);
    root_=0; index_=Spaces; generation_=0;
}
Lease acquire(uint64_t eptp,uint32_t cpu) {
    Runtime::Guard guard(gate);
    if (!guard || cpu>=128) return {};
    auto s=find(eptp,true);
    if (!s || s->quarantined || __atomic_load_n(&s->updating,__ATOMIC_ACQUIRE)) return {};
    __atomic_add_fetch(&s->references,1U,__ATOMIC_RELAXED);
    s->cpus[cpu/64] |= 1ULL<<(cpu%64);
    return Lease(size_t(s-spaces),s->root.pa,s->generation);
}
Update::~Update() {reset();}
Update::Update(Update &&other) noexcept:index_(other.index_),key_(other.key_) {other.index_=Spaces;}
Update &Update::operator=(Update &&other) noexcept {
    if(this!=&other) {reset();index_=other.index_;key_=other.key_;other.index_=Spaces;other.key_={};}
    return *this;
}
void Update::reset() {
    if(index_<Spaces) __atomic_store_n(&spaces[index_].updating,uint8_t(0),__ATOMIC_RELEASE);
    index_=Spaces;
}
Update beginUpdate(uint64_t eptp) {
    Runtime::Guard guard(gate);
    if(!guard) return {};
    auto s=find(eptp,true);
    if(!s || !flush(*s)) return {};
    clearSpace(*s);
    __atomic_store_n(&s->updating,uint8_t(1),__ATOMIC_RELEASE);
    return Update(size_t(s-spaces),s->key);
}
bool retire(Update &update) {
    Runtime::Guard guard(gate);
    if(!guard || !update) return false;
    auto &s=spaces[update.index_];
    if(!s.live || __atomic_load_n(&s.owners,__ATOMIC_ACQUIRE) || !__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE) ||
       (s.key && (!source.canRetire || !source.canRetire(source.context,s.key)))) return false;
    s.live=false;s.key={};s.eptp=0;update.key_={};return true;
}
#ifndef AMDHV_KERNEL_BUILD
uint64_t getOrCreateNCR3(uint64_t eptp,uint64_t &generation) {
    init(); Runtime::Guard guard(gate);
    auto s=guard?find(eptp,true):nullptr;
    generation=s?s->generation:0; return s?s->root.pa:0;
}
#endif
uint64_t convertEPTEntryToNPT(uint64_t e,bool leaf,bool large) {
    uint64_t permissions=e&7;
    if (!(permissions&EPT_READ) || (e&~(PHYS_ADDR_MASK|0x3ffULL))) return 0;
    if (leaf ? (((e&EPT_MEM_MASK)>>3)!=6 || (e&(1ULL<<6))) : (e&0xf8)!=0) return 0;
    if (!large && leaf && (e&EPT_LARGE_PAGE)) return 0;
    uint64_t n=(e&PHYS_ADDR_MASK)|NPT_PRESENT|NPT_USER|NPT_ACCESSED;
    if (e&EPT_WRITE) n|=NPT_RW|(leaf?NPT_DIRTY:0);
    if (!(e&EPT_EXEC)) n|=NPT_NX;
    if (large) n|=NPT_PS;
    return n;
}
uint64_t convertNPFInfoToEPTQualification(uint64_t info,uint64_t e) {
    uint64_t q=info&SVM_NPF_INFO1_WRITE?2:0;
    if (info&SVM_NPF_INFO1_FETCH) q|=4;
    if (!(q&6)) q|=1;
    if (info&SVM_NPF_INFO1_PRESENT) q|=(e&7)<<3;
    // Intel bit 8 is defined only when bit 7 establishes a valid GLA.
    return q;
}
class SourceLease {
    bool held_;
public:
    explicit SourceLease(uint64_t eptp,const SpaceKey &key={}):held_(key?
        (source.beginOwned && source.beginOwned(source.context,key)):(source.begin && source.begin(source.context,eptp))) {}
    ~SourceLease() { if(held_) source.end(source.context); }
    SourceLease(const SourceLease &)=delete;
    SourceLease &operator=(const SourceLease &)=delete;
    explicit operator bool() const {return held_;}
};
static bool readEntry(uint64_t pa,size_t index,uint64_t &value) {
    return pa && !(pa&~PHYS_ADDR_MASK) && index<512 &&
        source.read(source.context,pa+index*8,&value,8);
}
struct Walk { uint64_t entries[4] {}, tables[4] {}, effective {}, hpa {}; unsigned depth {}; };
static SyncResult walk(uint64_t eptp,uint64_t gpa,Walk &out,bool requireLeafOwnership=true) {
    if (!validEPTP(eptp) || gpa>=(1ULL<<48)) return SyncResult::Unsupported;
    uint64_t table=eptp&PHYS_ADDR_MASK, permissions=7;
    for (unsigned level=4;level;--level) {
        unsigned d=4-level; uint64_t entry=0;
        if (!readEntry(table,(gpa>>(12+9*(level-1)))&511,entry)) return SyncResult::Unowned;
        if (!(entry&7)) return SyncResult::NotMapped;
        bool large=(entry&EPT_LARGE_PAGE)!=0;
        bool leaf=level==1 || large;
        if ((large && (level==4 || level==1)) || !convertEPTEntryToNPT(entry,leaf,large)) return SyncResult::Unsupported;
        out.entries[d]=entry; out.tables[d]=table; permissions &= entry&7;
        if (leaf) {
            uint64_t offsetMask=(1ULL<<(12+9*(level-1)))-1;
            if ((entry&PHYS_ADDR_MASK)&offsetMask) return SyncResult::Unsupported;
            uint64_t hpa=(entry&PHYS_ADDR_MASK)|(gpa&offsetMask);
            // Shadow only the wired 4 KiB page, including source large leaves.
            // Proving one fault page must never expose its entire 1 GiB region.
            if(requireLeafOwnership && !source.allowMapping(source.context,eptp,gpa&~4095ULL,hpa&~4095ULL,4096,uint8_t(permissions))) return SyncResult::Unowned;
            out.effective=(entry&~7ULL)|permissions;
            out.hpa=(entry&PHYS_ADDR_MASK)|(gpa&offsetMask);
            out.depth=d+1; return SyncResult::Synced;
        }
        table=entry&PHYS_ADDR_MASK;
    }
    return SyncResult::Unsupported;
}
static Page *shadow(Space &s,uint64_t prefix,uint8_t level) {
    // Source tables can be aliased at different GPAs. Cache by destination path,
    // otherwise one alias could publish leaves proved only for another alias.
    uint64_t key=prefix|1;
    for (size_t i=0;i<capacity;++i) if (s.pages[i].source==key && s.pages[i].level==level) return &s.pages[i];
    for (size_t i=0;i<capacity;++i) if (!s.pages[i].source) {
        auto &p=s.pages[i]; memset(p.va,0,4096);p.source=key;p.level=level;return &p;
    }
    return nullptr;
}
static SyncResult sync(Space &s,uint64_t gpa,uint64_t *effective) {
    if (__atomic_load_n(&s.references,__ATOMIC_ACQUIRE) || __atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) return SyncResult::Busy;
    if (s.quarantined || !flush(s)) return SyncResult::ShootdownFailure;
    SourceLease owned(s.eptp,s.key);
    if (!owned) {s.quarantined=true;clearSpace(s);return SyncResult::Unowned;}
    Walk w;
    size_t i4=(gpa>>39)&511;
    // Revoke before walking. A failed/unsupported update cannot leave an old
    // permissive parent accessible. Caller observes failure, never success.
    s.root.va[i4]=0; s.generation=++clock;
    auto walked=walk(s.eptp,gpa,w);
    if(walked!=SyncResult::Synced) {
        if(walked!=SyncResult::NotMapped) s.quarantined=true;
        return walked;
    }
    if (effective) *effective=w.effective;
    Page *nodes[4]={&s.root,nullptr,nullptr,nullptr};
    for (unsigned d=1;d<4;++d) {
        uint64_t prefix=gpa&~((1ULL<<(48-9*d))-1);
        nodes[d]=shadow(s,prefix,uint8_t(4-d));
        if (!nodes[d]) return SyncResult::NoResources;
    }
    for (unsigned d=4;d-- >0;) {
        size_t index=(gpa>>(39-9*d))&511;
        bool leaf=d==3;
        uint64_t entry=leaf?convertEPTEntryToNPT((w.effective&~(PHYS_ADDR_MASK|EPT_LARGE_PAGE))|(w.hpa&PHYS_ADDR_MASK),true):
            nodes[d+1]->pa|NPT_PRESENT|NPT_RW|NPT_USER;
        nodes[d]->va[index]=entry;
    }
    return SyncResult::Synced;
}
SyncResult synchronize(uint64_t eptp,uint64_t gpa,uint64_t *effective) {
    if (effective) *effective=0;
    if (gpa>=(1ULL<<48) || !validEPTP(eptp)) return SyncResult::Unsupported;
#ifndef AMDHV_KERNEL_BUILD
    init();
#endif
    Runtime::Guard guard(gate);
    if(!guard) return SyncResult::Busy;
    auto s=find(eptp,true);
    return s?sync(*s,gpa,effective):SyncResult::NoResources;
}
SyncResult synchronize(const SpaceKey &key,uint64_t gpa,uint64_t *effective) {
    if(effective) *effective=0;
    if(gpa>=(1ULL<<48)) return SyncResult::Unsupported;
    Runtime::Guard guard(gate);if(!guard) return SyncResult::Busy;
    auto s=findOwned(key);return s?sync(*s,gpa,effective):SyncResult::Unowned;
}
SyncResult inspectMapping(const SpaceKey &key,const Update &revoked,uint64_t gpa,Mapping &out) {
    out={};if(gpa>=(1ULL<<48)) return SyncResult::Unsupported;
    Runtime::Guard guard(gate);if(!guard) return SyncResult::Busy;
    auto s=revoked.matches(key)?findOwned(key):nullptr;
    if(!s || !__atomic_load_n(&s->updating,__ATOMIC_ACQUIRE)) return SyncResult::Unowned;
    SourceLease owned(s->eptp,s->key);if(!owned) return SyncResult::Unowned;
    Walk w;auto result=walk(s->eptp,gpa,w,false);
    if(result==SyncResult::Synced) out={gpa&~4095ULL,w.hpa&~4095ULL,uint8_t(w.effective&7)};
    return result;
}
bool syncGPAFromEPT(uint64_t eptp,uint64_t gpa,uint64_t *effective) {
    return synchronize(eptp,gpa,effective)==SyncResult::Synced;
}
bool syncGPARangeFromEPT(uint64_t eptp,uint64_t gpa,uint64_t size) {
    if (!size || gpa>=(1ULL<<48) || size>(1ULL<<48)-gpa) return false;
    // Lazy rebuild after complete revocation avoids unbounded page-by-page work.
    return invalidateEPTP(eptp);
}
bool invalidateEPTP(uint64_t eptp) {
    Runtime::Guard guard(gate);
    if (!guard || (eptp && !validEPTP(eptp))) return false;
    // A source hook may execute INVEPT inside its own update ticket. That root
    // was synchronously revoked before the source operation, and admission is
    // still closed. Acknowledge without destroying the ticket or its proof.
    for (auto &s:spaces) if (s.live && (!eptp || s.eptp==eptp)) {
        if(__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) {
            if(__atomic_load_n(&s.references,__ATOMIC_ACQUIRE) || s.cpus[0] || s.cpus[1] || s.quarantined) return false;
        } else if(!flush(s)) return false;
    }
    for (auto &s:spaces) if (s.live && (!eptp || s.eptp==eptp) && !__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE)) clearSpace(s);
    return true;
}
bool acknowledgeRevoked(uint64_t eptp) {
    Runtime::Guard guard(gate);if(!guard || !prepared || (eptp && !validEPTP(eptp))) return false;
    bool found=!eptp;
    for(auto &s:spaces) if(s.live && (!eptp || s.eptp==eptp)) {
        found=true;
        if(!s.key || !__atomic_load_n(&s.updating,__ATOMIC_ACQUIRE) || __atomic_load_n(&s.references,__ATOMIC_ACQUIRE) ||
           s.cpus[0] || s.cpus[1] || s.quarantined) return false;
    }
    return found;
}
bool releaseEPTP(uint64_t eptp) {
    Runtime::Guard guard(gate);
    if (!guard || !validEPTP(eptp)) return false;
    for(auto &s:spaces) if(s.live && s.eptp==eptp && s.key) return false;
    auto s=find(eptp,false);
    if (!s) return true;
    if (!flush(*s)) return false;
    clearSpace(*s);s->live=false;s->eptp=0;return true;
}
static bool readPhysical(uint64_t eptp,const SpaceKey &key,uint64_t gpa,void *dst,size_t length) {
    SourceLease owned(eptp,key);if(!owned) return false;
    auto bytes=static_cast<uint8_t *>(dst);
    while(length) {
        Walk w;if(walk(eptp,gpa,w)!=SyncResult::Synced || !(w.effective&EPT_READ)) return false;
        size_t chunk=4096-(gpa&4095);if(chunk>length) chunk=length;
        if(!source.read(source.context,w.hpa,bytes,chunk)) return false;
        bytes+=chunk;gpa+=chunk;length-=chunk;
    }
    return true;
}
bool readGuestPhysical(const SpaceKey &key,uint64_t gpa,void *dst,size_t length) {
    if(!dst || !key || gpa>=(1ULL<<48) || length>(1ULL<<48)-gpa) return false;
    Runtime::Guard guard(gate);auto s=guard?findOwned(key):nullptr;
    if(!s || s->quarantined || __atomic_load_n(&s->updating,__ATOMIC_ACQUIRE)) return false;
    return readPhysical(s->eptp,s->key,gpa,dst,length);
}
bool readGuestPhysical(uint64_t eptp,uint64_t gpa,void *dst,size_t length) {
#if defined(AMDHV_KERNEL_BUILD)
    (void)eptp;(void)gpa;(void)dst;(void)length;return false;
#else
    if (!dst || !validEPTP(eptp) || gpa>=(1ULL<<48) || length>(1ULL<<48)-gpa) return false;
    Runtime::Guard guard(gate);
    if (!guard) return false;
    for(auto &s:spaces) if(s.live && s.eptp==eptp && s.key) return false;
    auto existing=find(eptp,false);
    if(existing && __atomic_load_n(&existing->updating,__ATOMIC_ACQUIRE)) return false;
    return readPhysical(eptp,{},gpa,dst,length);
#endif
}
}
