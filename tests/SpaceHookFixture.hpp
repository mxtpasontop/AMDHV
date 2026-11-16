#pragma once
#include "PhysicalFixture.hpp"
#include <algorithm>
struct AMDHVHookTestAccess {
    static constexpr auto init=&AMDHVPlugin::hookedSpaceInit;
    static constexpr auto map=&AMDHVPlugin::hookedSpaceMap;
    static constexpr auto mapKernel=&AMDHVPlugin::hookedSpaceMapKernel;
    static constexpr auto protect=&AMDHVPlugin::hookedSpaceProtect;
    static constexpr auto unmap=&AMDHVPlugin::hookedSpaceUnmap;
    static constexpr auto fault=&AMDHVPlugin::hookedSpaceEptFault;
    static constexpr auto invalidate=&AMDHVPlugin::hookedSpaceInvalidate;
    static constexpr auto free=&AMDHVPlugin::hookedSpaceFree;
    static constexpr auto vcpuInit=&AMDHVPlugin::hookedVcpuInitVMCS;
    static constexpr auto vcpuFree=&AMDHVPlugin::hookedVcpuFree;
};
struct SpaceHookFixture {
    static constexpr uintptr_t Address=0xffff800000200000ULL;
    static SpaceHookFixture *active;
    AppleHVSpace object {};
    uint64_t *p4 {},*p3 {},*p2 {},*p1 {};
    PhysicalFixture tables;
    bool wired[32] {},present[32] {},locked=false,allowFlush=true,failUnwire=false,failInit=false;
    uintptr_t thread=1;
    int references=0,originalUnmaps=0,originalProtects=0,originalFrees=0;
    std::vector<std::string> events;
    void *extraContext {};
    bool (*extraRead)(void *,uintptr_t,void *,size_t) {};
    static uint64_t *page() {void *p=nullptr;CHECK(!posix_memalign(&p,4096,4096));memset(p,0,4096);return static_cast<uint64_t *>(p);}
    SpaceHookFixture() {
        p4=page();p3=page();p2=page();p1=page();
        tables.add(p4);tables.add(p3);tables.add(p2);tables.add(p1);
        p4[0]=reinterpret_cast<uint64_t>(p3)|7;p3[0]=reinterpret_cast<uint64_t>(p2)|7;p2[0]=reinterpret_cast<uint64_t>(p1)|7;
        active=this;
    }
    ~SpaceHookFixture() {std::free(p4);std::free(p3);std::free(p2);std::free(p1);active=nullptr;}
    static AppleHVSpace *address() {return reinterpret_cast<AppleHVSpace *>(Address);}
    AppleHVABI::Space snapshot() const {
        AppleHVABI::Space s;s.address=Address;s.map=0x123;s.pmap=0x4000;s.task=0x5000;
        s.rootPA=reinterpret_cast<uint64_t>(p4);s.eptp=s.rootPA|0x1e;return s;
    }
    bool configure() {
        XNUPlatform::Services native;
        native.cpu=[] {return 0U;};native.thread=[] {return active?active->thread:0;};
        native.rendezvous=[](void (*action)(void *),void *arg) {action(arg);};
        native.readMSR=[](uint32_t,uint64_t *) {return 1;};native.writeMSR=[](uint32_t,uint64_t) {return 1;};
        native.copyNoFault=[](uintptr_t source,uintptr_t out,size_t size)->size_t {
            if(active && active->extraRead && active->extraRead(active->extraContext,source,reinterpret_cast<void *>(out),size)) return size;
            if(!active || source<Address || source-Address>sizeof(object) || size>sizeof(object)-(source-Address)) return 0;
            memcpy(reinterpret_cast<void *>(out),reinterpret_cast<uint8_t *>(&active->object)+(source-Address),size);return size;
        };
        if(!XNUPlatform::configure(native)) return false;
        GuestMemory::Operations memory{this,
            [](void *p,uintptr_t map) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(map==0x123);++m.references;m.events.emplace_back("retain");},
            [](void *p,uintptr_t map) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(map==0x123 && --m.references==0);m.events.emplace_back("release");},
            [](void *p,uintptr_t map,uint64_t gpa,uint32_t,uint64_t &pa) {
                auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(map==0x123 && gpa/4096<32 && !m.wired[gpa/4096] && m.present[gpa/4096]);
                m.wired[gpa/4096]=true;pa=0x100000+gpa;m.events.emplace_back("wire");return 0;
            },
            [](void *p,uintptr_t map,uint64_t first,uint64_t end) {
                auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(map==0x123 && first<end && end/4096<=32);
                if(m.failUnwire) return 1;
                for(auto page=first/4096;page<end/4096;++page) {CHECK(m.wired[page]);m.wired[page]=false;}
                m.events.emplace_back("unwire");return 0;
            },
            [](void *,uint64_t,void *out,size_t size) {memset(out,0x5a,size);return true;}};
        EPTSource::Operations source{this,
            [](void *p,uintptr_t pmap) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(pmap==0x4000 && !m.locked);m.locked=true;return true;},
            [](void *p,uintptr_t pmap) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(pmap==0x4000 && m.locked);m.locked=false;},
            [](void *p,uintptr_t,uint64_t &root) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(m.locked);root=m.snapshot().eptp;return true;},
            [](void *p,uint64_t pa,void *out,size_t size) {auto &m=*static_cast<SpaceHookFixture *>(p);CHECK(m.locked);return PhysicalFixture::read(&m.tables,pa,out,size);}};
        return SpaceLifecycle::global().configure(memory,source,{this,[](void *p,uint64_t,uint64_t,const uint64_t[2]) {
            auto &m=*static_cast<SpaceHookFixture *>(p);m.events.emplace_back("flush");return m.allowFlush;
        }},[](void *p) {return static_cast<SpaceHookFixture *>(p)->thread;},this);
    }
    static bool init(AppleHVSpace *space,void *) {
        CHECK(space==address());auto &m=*active;m.object={};
        if(m.failInit) return false;
        m.object.vm_map=reinterpret_cast<void *>(0x123);m.object.pmap=reinterpret_cast<void *>(0x4000);
        m.object.eptRootAndFlags=m.snapshot().eptp;m.object.task=reinterpret_cast<void *>(0x5000);return true;
    }
    static int map(AppleHVSpace *space,uint64_t,uint64_t gpa,unsigned long size,uint64_t flags) {
        auto &m=*active;m.events.emplace_back("map-start");
        for(auto page=gpa/4096;page<(gpa+size)/4096;++page) m.present[page]=true;
        int result=AMDHVHookTestAccess::protect(space,gpa,size,flags);m.events.emplace_back("map-end");return result;
    }
    static int mapKernel(AppleHVSpace *space,uint64_t hva,uint64_t gpa,unsigned long size,int flags) {return map(space,hva,gpa,size,flags);}
    static int protect(AppleHVSpace *,uint64_t gpa,unsigned long size,uint64_t flags) {
        auto &m=*active;++m.originalProtects;m.events.emplace_back("protect");
        for(auto page=gpa/4096;page<(gpa+size)/4096;++page) {CHECK(!m.wired[page]);if(m.p1[page]) m.p1[page]=(m.p1[page]&~7ULL)|(flags&7);}
        return 0;
    }
    static int fault(AppleHVSpace *,uint64_t gpa,uint64_t,bool) {
        auto &m=*active;m.events.emplace_back("fault");if(!m.present[gpa/4096]) return 1;
        m.p1[gpa/4096]=(0x100000+(gpa&~4095ULL))|0x37;return 0;
    }
    static int unmap(AppleHVSpace *,uint64_t gpa,unsigned long size) {
        auto &m=*active;++m.originalUnmaps;m.events.emplace_back("unmap");
        for(auto page=gpa/4096;page<(gpa+size)/4096;++page) {CHECK(!m.wired[page]);m.present[page]=false;m.p1[page]=0;}
        return 0;
    }
    static void invalidate(AppleHVSpace *) {CHECK(NPTManager::invalidateEPTP(active->snapshot().eptp));}
    static void free(AppleHVSpace *) {
        auto &m=*active;CHECK(!m.references);for(bool wired:m.wired) CHECK(!wired);
        ++m.originalFrees;m.events.emplace_back("free");memset(&m.object,0xa5,sizeof(m.object));
    }
};
SpaceHookFixture *SpaceHookFixture::active=nullptr;
struct VcpuHookFixture {
    static constexpr uint64_t Address=0xffff800000300000ULL,VM=Address+0x1000;
    uint8_t object[0x398] {},vm[0x288] {};
    uint8_t *rw {},*ro {},*cs {};
    SpaceHookFixture &space;
    int references[3] {},inits {},frees {};
    bool failOriginal {};
    static VcpuHookFixture *active;
    template<class T> void put(size_t offset,T value) {memcpy(object+offset,&value,sizeof(value));}
    static bool read(void *context,uintptr_t address,void *out,size_t bytes) {
        auto &m=*static_cast<VcpuHookFixture *>(context);
        struct Span {uint64_t address;const uint8_t *data;size_t bytes;};
        Span spans[]={{Address,m.object,sizeof(m.object)},{VM,m.vm,sizeof(m.vm)},
            {reinterpret_cast<uint64_t>(m.cs),m.cs,4096}};
        for(auto s:spans) if(address>=s.address && address-s.address<=s.bytes && bytes<=s.bytes-(address-s.address)) {
            memcpy(out,s.data+(address-s.address),bytes);return true;
        }
        return false;
    }
    explicit VcpuHookFixture(SpaceHookFixture &source):space(source) {
        rw=reinterpret_cast<uint8_t *>(SpaceHookFixture::page());ro=reinterpret_cast<uint8_t *>(SpaceHookFixture::page());cs=reinterpret_cast<uint8_t *>(SpaceHookFixture::page());
        active=this;space.extraContext=this;space.extraRead=read;
        put(0x28,VM);put(0x30,uint64_t(SpaceHookFixture::Address));put(0x38,reinterpret_cast<uint64_t>(rw));put(0x40,reinterpret_cast<uint64_t>(ro)+19);
        uintptr_t descriptor=1;memcpy(vm+0x280,&descriptor,8);descriptor=2;memcpy(vm+0x270,&descriptor,8);
        VcpuLifecycle::Operations ops;ops.context=this;ops.memory={this,
            [](void *p,uint64_t a,void *out,size_t n) {return read(p,uintptr_t(a),out,n);},nullptr,nullptr};
        ops.thread=[](void *p) {return static_cast<VcpuHookFixture *>(p)->space.thread;};
        ops.retain=[](void *p,uintptr_t descriptor) {CHECK(descriptor>=1 && descriptor<=3);++static_cast<VcpuHookFixture *>(p)->references[descriptor-1];return true;};
        ops.release=[](void *p,uintptr_t descriptor) {CHECK(descriptor>=1 && descriptor<=3);CHECK(--static_cast<VcpuHookFixture *>(p)->references[descriptor-1]==0);};
        ops.inspect=[](void *p,uintptr_t descriptor,VcpuLifecycle::Buffer &out) {
            auto &m=*static_cast<VcpuHookFixture *>(p);CHECK(descriptor>=1 && descriptor<=3 && m.references[descriptor-1]);
            out={reinterpret_cast<uint64_t>(descriptor==1?m.rw:descriptor==2?m.ro:m.cs),4096};return true;
        };
        CHECK(VcpuLifecycle::global().configure(ops));
        orgVcpuInitVMCS=reinterpret_cast<mach_vm_address_t>(&init);
        orgVcpuFree=reinterpret_cast<mach_vm_address_t>(&free);
    }
    static bool init(void *) {
        auto &m=*active;++m.inits;if(m.failOriginal) return false;
        m.put(0x88,uint64_t(3));m.put(0x90,uint64_t(0x5000));uint32_t revision=1;memcpy(m.cs,&revision,4);
        auto region=VcpuLifecycle::global().acquirePhysical(0x5000);CHECK(region);
        auto core=SoftVMCS::findByPhysAddr(0x5000);CHECK(core && SoftVMCS::setActiveVMCS(0,core));
        core.reset();CHECK(SoftVMCS::clearActiveVMCS(0));return true;
    }
    static void free(void *) {
        auto &m=*active;++m.frees;
        CHECK(m.references[0]==1 && m.references[1]==1);
        CHECK(!SoftVMCS::findByPhysAddr(0x5000));memset(m.object,0xa5,sizeof(m.object));
    }
    ~VcpuHookFixture() {
        CHECK(VcpuLifecycle::global().empty());for(auto n:references) CHECK(!n);
        space.extraRead=nullptr;space.extraContext=nullptr;active=nullptr;std::free(rw);std::free(ro);std::free(cs);
    }
};
VcpuHookFixture *VcpuHookFixture::active=nullptr;
static void spaceHookIntegration() {
    CHECK(NPTManager::shutdown() && NPTManager::prepareResources(16));
    SpaceHookFixture m;CHECK(m.configure());
    CHECK(SpaceLifecycle::global().ready());
    orgSpaceInit=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::init);
    orgSpaceMap=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::map);
    orgSpaceMapKernel=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::mapKernel);
    orgSpaceProtect=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::protect);
    orgSpaceEptFault=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::fault);
    orgSpaceUnmap=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::unmap);
    orgSpaceInvalidate=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::invalidate);
    orgSpaceFree=reinterpret_cast<mach_vm_address_t>(&SpaceHookFixture::free);
    using H=AMDHVHookTestAccess;auto object=SpaceHookFixture::address();auto snapshot=m.snapshot();auto &context=SpaceLifecycle::global();
    m.failInit=true;CHECK(!H::init(object,nullptr));H::free(object);CHECK(m.originalFrees==1);
    m.failInit=false;CHECK(H::init(object,nullptr) && m.references==1);
    auto first=context.lookup(snapshot);CHECK(first);
    {
        CHECK(SoftVMCS::shutdown() && SoftVMCS::prepareResources(2,8));VcpuHookFixture vcpu(m);
        CHECK(H::vcpuInit(reinterpret_cast<void *>(VcpuHookFixture::Address)) && vcpu.inits==1);
        auto metadata=VcpuLifecycle::global().acquire(VcpuHookFixture::Address);CHECK(metadata);
        H::free(object);CHECK(m.references==1 && m.originalFrees==1); // vCPU pins source lifetime
        H::vcpuFree(reinterpret_cast<void *>(VcpuHookFixture::Address));CHECK(!vcpu.frees);
        metadata.reset();H::vcpuFree(reinterpret_cast<void *>(VcpuHookFixture::Address));CHECK(vcpu.frees==1);
        CHECK(VcpuLifecycle::global().empty() && NPTManager::hasNoOwners(first));
        vcpu.put(0x28,VcpuHookFixture::VM);vcpu.put(0x30,uint64_t(SpaceHookFixture::Address));
        vcpu.put(0x38,reinterpret_cast<uint64_t>(vcpu.rw));vcpu.put(0x40,reinterpret_cast<uint64_t>(vcpu.ro)+19);
        vcpu.failOriginal=true;CHECK(!H::vcpuInit(reinterpret_cast<void *>(VcpuHookFixture::Address)));
        H::vcpuFree(reinterpret_cast<void *>(VcpuHookFixture::Address));CHECK(vcpu.frees==2);
        CHECK(SoftVMCS::shutdown());gHookFailure=false;
    }
    CHECK(!H::map(object,0x8000,0,8192,7) && m.originalProtects==1 && !m.wired[0] && !m.wired[1]);
    CHECK(!H::fault(object,0,1,false) && m.wired[0]);
    CHECK(NPTManager::synchronize(first,0)==NPTManager::SyncResult::Synced);
    CHECK(!H::fault(object,4096,1,false) && m.wired[1]);
    auto lease=NPTManager::acquire(first,0);CHECK(lease);lease.reset();m.events.clear();
    CHECK(!H::unmap(object,4096,4096));
    CHECK(m.events.size()==3 && m.events[0]=="flush" && m.events[1]=="unwire" && m.events[2]=="unmap");
    CHECK(m.wired[0] && !m.wired[1]);
    {auto mutation=context.begin(snapshot);CHECK(mutation);m.thread=2;
     CHECK(H::protect(object,0,4096,1)==SpaceBackendError);m.thread=1;
     H::invalidate(object);CHECK(!NPTManager::acquire(first,0));}
    CHECK(!H::protect(object,0,4096,1) && !m.wired[0]);
    H::free(object);CHECK(m.references==0 && context.empty() && m.originalFrees==2);
    CHECK(H::init(object,nullptr));auto next=context.lookup(snapshot);CHECK(next && next.lifetime()!=first.lifetime());
    CHECK(NPTManager::synchronize(first,0)==NPTManager::SyncResult::Unowned);
    CHECK(!H::mapKernel(object,0xffff800000100000ULL,0,8192,7));
    CHECK(!H::fault(object,0,1,false));
    lease=NPTManager::acquire(next,1);CHECK(lease);lease.reset();
    m.allowFlush=false;int oldUnmaps=m.originalUnmaps;
    CHECK(H::unmap(object,0,4096)==SpaceBackendError && m.originalUnmaps==oldUnmaps && m.wired[0]);
    m.allowFlush=true;m.failUnwire=true;
    CHECK(H::unmap(object,0,4096)==SpaceBackendError && m.originalUnmaps==oldUnmaps && m.wired[0]);
    CHECK(!context.healthy() && !context.ready());H::free(object);CHECK(m.references==1 && m.originalFrees==2);
    m.failUnwire=false;H::free(object);CHECK(m.references==0 && m.originalFrees==3 && context.empty());
    CHECK(NPTManager::shutdown());
}
