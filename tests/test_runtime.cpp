#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <src/Memory.hpp>
#include <src/SoftVMCS.hpp>
#include <src/NPTManager.hpp>
#include <src/HostLifecycle.hpp>
#include <src/VMCBTranslator.hpp>
#include <src/Architecture.hpp>
#include <src/EPTSource.hpp>
#include <src/SpaceLifecycle.hpp>
#include "PhysicalFixture.hpp"
bool amdhvDebugEnabled=false;
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"CHECK %s at %d\n",#x,__LINE__);abort();}} while(0)
#include "VcpuFixture.hpp"
#include "TrapFixture.hpp"
static void failureInjection() {
    CHECK(SoftVMCS::shutdown());CHECK(NPTManager::shutdown());
    CHECK(Memory::outstanding()==0);
    for(int i=0;i<7;++i) {
        Memory::failAfter(i);
        CHECK(!SoftVMCS::prepareResources(2,4));
        CHECK(Memory::outstanding()==0);
    }
    Memory::failAfter(7);CHECK(SoftVMCS::prepareResources(2,4));
    CHECK(SoftVMCS::getHostSavePA(1));CHECK(!SoftVMCS::getHostSavePA(2));
    CHECK(SoftVMCS::shutdown());CHECK(Memory::outstanding()==0);
    for(int i=0;i<12;++i) {
        Memory::failAfter(i);CHECK(!NPTManager::prepareResources(2));
        CHECK(Memory::outstanding()==0);
    }
    Memory::failAfter(12);CHECK(NPTManager::prepareResources(2));
    CHECK(NPTManager::shutdown());CHECK(Memory::outstanding()==0);
    Memory::failAfter(-1);
}
static void ownership() {
    CHECK(!SoftVMCS::prepareResources(129,4));
    CHECK(!SoftVMCS::prepareResources(1,1));
    CHECK(SoftVMCS::prepareResources(2,4));
    auto a=SoftVMCS::getOrCreateByPhysAddr(0x1000);
    auto b=SoftVMCS::getOrCreateByPhysAddr(0x2000);
    auto c=SoftVMCS::getOrCreateByPhysAddr(0x3000);
    CHECK(a && b && c && a->asid==1 && b->asid==2 && c->asid==3);
    CHECK(!SoftVMCS::getOrCreateByPhysAddr(0x4000));
    CHECK(!SoftVMCS::findByPhysAddr(0x1000));
    CHECK(!SoftVMCS::releaseByPhysAddr(0x1000));
    CHECK(!SoftVMCS::shutdown());
    CHECK(SoftVMCS::setActiveVMCS(0,a));
    CHECK(!SoftVMCS::setActiveVMCS(1,a));
    a.reset();CHECK(!SoftVMCS::releaseByPhysAddr(0x1000));
    CHECK(SoftVMCS::clearActiveVMCS(0));
    CHECK(SoftVMCS::releaseByPhysAddr(0x1000));
    auto recycled=SoftVMCS::getOrCreateByPhysAddr(0x4000);
    CHECK(recycled && recycled->lifetime>b->lifetime && !recycled->lastRoot && recycled->lastCPU==128);
    recycled.reset();b.reset();c.reset();CHECK(SoftVMCS::shutdown());
    CHECK(SoftVMCS::prepareResources(12,65));
    const size_t allocations=Memory::outstanding();
    std::atomic<unsigned> operations{0};
    std::vector<std::thread> workers;
    for(unsigned t=0;t<12;++t) workers.emplace_back([&] {
        for(unsigned i=0;i<2000;++i) {
            auto v=SoftVMCS::getOrCreateByPhysAddr(0x100000);
            if(!v) {std::this_thread::yield();continue;}
            uint64_t lifetime=v->lifetime;
            CHECK(v->inUse && !SoftVMCS::releaseByPhysAddr(0x100000));
            CHECK(SoftVMCS::writeField(v.get(),VMCS_GUEST_RIP,lifetime));
            std::this_thread::yield();
            uint64_t observed=0;
            CHECK(SoftVMCS::readField(v.get(),VMCS_GUEST_RIP,observed) && observed==lifetime);
            ++operations;
            v.reset();SoftVMCS::releaseByPhysAddr(0x100000);
        }
    });
    for(auto &w:workers) w.join();
    CHECK(operations>0 && Memory::outstanding()==allocations);
    CHECK(SoftVMCS::shutdown());CHECK(Memory::outstanding()==0);
}
static uint64_t *page() {
    void *p=nullptr;CHECK(!posix_memalign(&p,4096,4096));memset(p,0,4096);return static_cast<uint64_t *>(p);
}
static void isolation() {
    CHECK(NPTManager::prepareResources(8));
    auto p4=page(),p3=page(),p2=page(),p1=page(),data=page();
    PhysicalFixture memory;memory.add(p4);memory.add(p3);memory.add(p2);memory.add(p1);memory.add(data);
    memory.allowHPA(reinterpret_cast<uint64_t>(data));
    memory.allowHPA(0x40000000,1ULL<<30);
    CHECK(memory.install());
    uint64_t eptp=reinterpret_cast<uint64_t>(p4)|0x1e;
    p4[0]=reinterpret_cast<uint64_t>(p3)|1; // Ancestor forbids writes and execution.
    p3[0]=reinterpret_cast<uint64_t>(p2)|7;
    p2[0]=reinterpret_cast<uint64_t>(p1)|7;
    p1[0]=reinterpret_cast<uint64_t>(data)|0x37;
    data[0]=0xdeadbeef;
    uint64_t effective=0;
    CHECK(NPTManager::syncGPAFromEPT(eptp,0,&effective));
    CHECK((effective&7)==1);
    uint64_t value=0;CHECK(NPTManager::readGuestPhysical(eptp,0,&value,8) && value==0xdeadbeef);
    CHECK(!NPTManager::readGuestPhysical(eptp,4092,&value,8));
    auto lease=NPTManager::acquire(eptp,127);
    CHECK(lease && !NPTManager::releaseEPTP(eptp));
    CHECK(!NPTManager::invalidateEPTP(eptp) && !NPTManager::syncGPAFromEPT(eptp,0));
    CHECK(!NPTManager::shutdown());
    lease.reset();memory.allowFlush=false;
    CHECK(!NPTManager::invalidateEPTP(eptp));
    CHECK(!NPTManager::acquire(eptp,0)); // Failed shootdown quarantines entry.
    memory.allowFlush=true;CHECK(NPTManager::invalidateEPTP(eptp));
    CHECK(memory.flushes==2);
    uint64_t generation=0;auto root=reinterpret_cast<uint64_t *>(NPTManager::getOrCreateNCR3(eptp,generation));
    CHECK(root && !root[0]);
    {
        auto update=NPTManager::beginUpdate(eptp);CHECK(update);
        CHECK(!NPTManager::acquire(eptp,0) && !NPTManager::releaseEPTP(eptp) && !NPTManager::shutdown());
        CHECK(NPTManager::synchronize(eptp,0)==NPTManager::SyncResult::Busy);
        CHECK(NPTManager::invalidateEPTP(eptp));
        CHECK(!NPTManager::acquire(eptp,0));
    }
    p4[0]=0x100000|7; // Unowned physical page cannot be followed.
    CHECK(!NPTManager::syncGPAFromEPT(eptp,0) && !root[0]);
    CHECK(NPTManager::invalidateEPTP(eptp));
    p4[0]=reinterpret_cast<uint64_t>(p3)|7;
    p3[0]=0x40000000|0xb7; // 1GB WB leaf.
    CHECK(NPTManager::syncGPAFromEPT(eptp,0));
    CHECK(NPTManager::invalidateEPTP(eptp) && !root[0]);
    p3[0]=0x40001000|0xb7; // Misaligned large leaf must fail.
    CHECK(!NPTManager::syncGPAFromEPT(eptp,0));
    CHECK(NPTManager::invalidateEPTP(eptp));
    p3[0]=reinterpret_cast<uint64_t>(p2)|7;
    p1[0]=reinterpret_cast<uint64_t>(data)|0x34; // Execute-only forbidden.
    CHECK(!NPTManager::syncGPAFromEPT(eptp,0));
    CHECK(NPTManager::invalidateEPTP(eptp));
    p1[0]=0x100000|0x37;
    CHECK(NPTManager::synchronize(eptp,0)==NPTManager::SyncResult::Unowned);
    CHECK(!NPTManager::acquire(eptp,0));
    CHECK(NPTManager::invalidateEPTP(eptp));
    p1[0]=reinterpret_cast<uint64_t>(data)|0x37;
    const size_t allocations=Memory::outstanding();
    std::vector<std::thread> workers;
    for(unsigned t=0;t<12;++t) workers.emplace_back([&,t] {
        for(unsigned i=0;i<1000;++i) {
            switch(i%3) {
                case 0:NPTManager::syncGPAFromEPT(eptp,0);break;
                case 1:NPTManager::invalidateEPTP(eptp);break;
                case 2:{auto pin=NPTManager::acquire(eptp,t);if(pin) CHECK(pin.root() && pin.generation());break;}
            }
        }
    });
    for(auto &w:workers) w.join();
    CHECK(Memory::outstanding()==allocations);
    CHECK(NPTManager::releaseEPTP(eptp));
    memory.ownedOwner=0xabc;
    auto original=NPTManager::registerSpace(0xabc,eptp);CHECK(original);
    CHECK(!NPTManager::registerSpace(0xabc,eptp));
    CHECK(!NPTManager::acquire(eptp,0)); // Raw roots cannot acquire owned spaces.
    CHECK(!NPTManager::releaseEPTP(eptp) && !NPTManager::readGuestPhysical(eptp,0,&value,8));
    auto ownedLease=NPTManager::acquire(original,0);CHECK(ownedLease && ownedLease.matches(original));
    CHECK(!NPTManager::releaseSpace(original));ownedLease.reset();
    CHECK(NPTManager::synchronize(original,0)==NPTManager::SyncResult::Synced);
    CHECK(NPTManager::readGuestPhysical(original,0,&value,8) && value==0xdeadbeef);
    CHECK(NPTManager::releaseSpace(original));
    auto replacement=NPTManager::registerSpace(0xabc,eptp);
    CHECK(replacement && replacement.lifetime()!=original.lifetime());
    CHECK(!NPTManager::acquire(original,0) && !NPTManager::releaseSpace(original));
    CHECK(NPTManager::synchronize(original,0)==NPTManager::SyncResult::Unowned);
    CHECK(!NPTManager::beginUpdate(original));
    auto current=NPTManager::acquire(replacement,0);CHECK(current && !current.matches(original));current.reset();
    CHECK(NPTManager::releaseSpace(replacement));CHECK(NPTManager::shutdown());
    CHECK(Memory::outstanding()==0);
    free(p4);free(p3);free(p2);free(p1);free(data);
}
struct MapModel {
    int references=0,wireCalls=0,failWire=-1,invalidPA=-1,unwireCalls=0,failUnwire=-1;
    bool pages[32] {};
    GuestMemory::Operations operations() {
        return {this,
            [](void *p,uintptr_t map) {CHECK(map==0x123);++static_cast<MapModel *>(p)->references;},
            [](void *p,uintptr_t map) {CHECK(map==0x123);CHECK(--static_cast<MapModel *>(p)->references==0);},
            [](void *p,uintptr_t map,uint64_t gpa,uint32_t,uint64_t &pa) {
                auto &m=*static_cast<MapModel *>(p);CHECK(map==0x123 && gpa/4096<32 && !m.pages[gpa/4096]);
                int call=m.wireCalls++;if(call==m.failWire) return 1;
                m.pages[gpa/4096]=true;pa=call==m.invalidPA?0:0x100000+gpa;return 0;
            },
            [](void *p,uintptr_t map,uint64_t first,uint64_t end) {
                auto &m=*static_cast<MapModel *>(p);CHECK(map==0x123 && first<end && !(first&4095) && !(end&4095) && end/4096<=32);
                if(m.unwireCalls++==m.failUnwire) return 1;
                for(auto page=first/4096;page<end/4096;++page) {CHECK(m.pages[page]);m.pages[page]=false;}
                return 0;
            },
            [](void *,uint64_t,void *out,size_t size) {memset(out,0x5a,size);return true;}};
    }
};
static void guestOwnership() {
    using R=GuestMemory::Result;
    CHECK(NPTManager::prepareResources(2));
    PhysicalFixture source;GuestMemory::Registry guest;MapModel model;
    source.guestMemory=&guest;source.ownedOwner=0xabc;CHECK(source.install());CHECK(guest.configure(model.operations()));
    auto key=NPTManager::registerSpace(source.ownedOwner,0x201e);CHECK(key);
    NPTManager::Update none;CHECK(guest.adopt(key,none,0x123)==R::Unowned);
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);
        CHECK(guest.adopt(key,update,0x123)==R::Success && model.references==1);
        CHECK(!NPTManager::retire(update)); // Retained source maps outlive the key.
        CHECK(guest.wireRange(key,update,0,8192,3)==R::Success);
        CHECK(guest.owns(key,0,0x100000,8192,3) && !guest.owns(key,0,0x100000,8192,7));
        CHECK(!guest.owns(key,0,0x101000,4096,1));
        uint64_t word=0;CHECK(guest.read(key,0x100000,&word,8) && word==0x5a5a5a5a5a5a5a5aULL);
        CHECK(!guest.read(key,0x102000,&word,8));
        CHECK(guest.wireRange(key,update,4096,4096,3)==R::InvalidRange);
        CHECK(guest.wireRange(key,update,1,4096,3)==R::InvalidRange);
        CHECK(guest.wireRange(key,update,(1ULL<<48)-4096,8192,3)==R::InvalidRange);
        // Roll back pages appended to an existing coalesced run.
        model.failWire=model.wireCalls+1;
        CHECK(guest.wireRange(key,update,8192,8192,3)==R::WireFailure);
        CHECK(model.pages[0] && model.pages[1] && !model.pages[2] && !model.pages[3]);
        CHECK(!guest.owns(key,8192,0x102000,4096,1));model.failWire=-1;
        model.invalidPA=model.wireCalls;
        CHECK(guest.wireRange(key,update,8192,4096,3)==R::WireFailure && !model.pages[2]);model.invalidPA=-1;
        // Failed rollback retains the wire and map but blocks publication/read.
        model.failWire=model.wireCalls+1;model.failUnwire=model.unwireCalls;
        CHECK(guest.wireRange(key,update,8192,8192,3)==R::UnwireFailure);
        CHECK(model.pages[2] && !guest.owns(key,0,0x100000,4096,1) && !guest.read(key,0x100000,&word,8));
        CHECK(guest.wireRange(key,update,16384,4096,1)==R::UnwireFailure);
        CHECK(!NPTManager::retire(update) && !guest.empty());
        model.failWire=-1;model.failUnwire=-1;
        CHECK(guest.release(key,update)==R::Success && model.references==0 && guest.empty());
        for(bool page:model.pages) CHECK(!page);
        CHECK(NPTManager::retire(update) && !update.matches(key));
        CHECK(guest.adopt(key,update,0x123)==R::Unowned);
    }
    // A stale lifetime cannot adopt or wire a replacement with the same root.
    auto next=NPTManager::registerSpace(source.ownedOwner,0x201e);CHECK(next && next.lifetime()!=key.lifetime());
    {
        auto update=NPTManager::beginUpdate(next);CHECK(update);
        CHECK(guest.adopt(key,update,0x123)==R::Unowned);
        CHECK(guest.adopt(next,update,0x123)==R::Success);
        CHECK(guest.wireRange(next,update,0,4096,1)==R::Success);
    }
    CHECK(!NPTManager::releaseSpace(next) && !NPTManager::shutdown());
    {
        auto update=NPTManager::beginUpdate(next);CHECK(update);
        model.failUnwire=model.unwireCalls;
        CHECK(guest.release(next,update)==R::UnwireFailure && model.references==1);
        CHECK(guest.release(next,update)==R::Success && model.references==0);
        CHECK(NPTManager::retire(update));
    }
    CHECK(NPTManager::shutdown());
}
static void wireRanges() {
    using R=GuestMemory::Result;
    CHECK(NPTManager::prepareResources(2));
    PhysicalFixture source;GuestMemory::Registry guest;MapModel model;
    source.guestMemory=&guest;source.ownedOwner=0xabc;CHECK(source.install());CHECK(guest.configure(model.operations()));
    auto key=NPTManager::registerSpace(0xabc,0x201e);CHECK(key);
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);
        CHECK(guest.adopt(key,update,0x123)==R::Success);
        CHECK(guest.wireRange(key,update,0,8*4096,3)==R::Success);
        CHECK(guest.unwireRange(key,update,2*4096,2*4096)==R::Success);
        CHECK(guest.owns(key,0,0x100000,8192,3) && guest.owns(key,4*4096,0x104000,4*4096,3));
        CHECK(!guest.owns(key,0,0x100000,8*4096,3) && !model.pages[2] && !model.pages[3]);
        CHECK(guest.unwireRange(key,update,4*4096,2*4096)==R::Success);
        CHECK(guest.unwireRange(key,update,0,2*4096)==R::Success);
        int calls=model.unwireCalls;
        CHECK(guest.unwireRange(key,update,0,6*4096)==R::Success && model.unwireCalls==calls);
        CHECK(guest.release(key,update)==R::Success && !model.references);
        for(bool page:model.pages) CHECK(!page);
        CHECK(guest.adopt(key,update,0x123)==R::Success);
        CHECK(guest.wireRange(key,update,0,8192,3)==R::Success);
        CHECK(guest.wireRange(key,update,4*4096,8192,3)==R::Success);
        model.failUnwire=model.unwireCalls+1;
        CHECK(guest.unwireRange(key,update,0,6*4096)==R::UnwireFailure);
        CHECK(!model.pages[0] && !model.pages[1] && model.pages[4] && model.pages[5]);
        CHECK(!guest.owns(key,4*4096,0x104000,8192,1));
        CHECK(guest.release(key,update)==R::Success);
        for(bool page:model.pages) CHECK(!page);
        CHECK(NPTManager::retire(update));
    }
    CHECK(NPTManager::shutdown());
}
static uint64_t *child(uint64_t entry) {return reinterpret_cast<uint64_t *>(entry&NPTManager::PHYS_ADDR_MASK);}
static void shadowAliasAndLargeLeaf() {
    CHECK(NPTManager::prepareResources(8));
    auto p4=page(),p3=page(),p2=page(),p1=page();
    PhysicalFixture physical;physical.add(p4);physical.add(p3);physical.add(p2);physical.add(p1);
    physical.allowHPA(0x20000000,8192);physical.allowHPA(0x40001000);physical.limitGPA=true;CHECK(physical.install());
    uint64_t eptp=reinterpret_cast<uint64_t>(p4)|0x1e,generation=0;
    p4[0]=p4[1]=reinterpret_cast<uint64_t>(p3)|7;p3[0]=reinterpret_cast<uint64_t>(p2)|7;
    p2[0]=reinterpret_cast<uint64_t>(p1)|7;p1[0]=0x20000000|0x37;p1[1]=0x20001000|0x37;
    CHECK(NPTManager::syncGPAFromEPT(eptp,0));
    uint64_t alias=1ULL<<39;physical.allowedGPA=alias+4096;
    CHECK(NPTManager::syncGPAFromEPT(eptp,alias+4096));
    auto root=reinterpret_cast<uint64_t *>(NPTManager::getOrCreateNCR3(eptp,generation));CHECK(root && child(root[0])!=child(root[1]));
    auto aliasPT=child(child(child(root[1])[0])[0]);
    CHECK(!aliasPT[0] && (aliasPT[1]&NPTManager::NPT_PRESENT));
    CHECK(NPTManager::invalidateEPTP(eptp));p4[1]=0;p3[0]=0x40000000|0xb7;physical.allowedGPA=4096;
    CHECK(NPTManager::syncGPAFromEPT(eptp,4096));
    auto n3=child(root[0]);CHECK(!(n3[0]&NPTManager::NPT_PS));
    auto n2=child(n3[0]);CHECK(!(n2[0]&NPTManager::NPT_PS));
    auto n1=child(n2[0]);CHECK(!n1[0] && (n1[1]&NPTManager::PHYS_ADDR_MASK)==0x40001000 && !n1[2]);
    CHECK(!NPTManager::syncGPAFromEPT(eptp,0));
    CHECK(NPTManager::releaseEPTP(eptp) && NPTManager::shutdown());
    free(p4);free(p3);free(p2);free(p1);
}
static void pinnedSource() {
    using R=GuestMemory::Result;
    CHECK(NPTManager::prepareResources(8));
    auto p4=page(),p3=page(),p2=page(),p1=page();
    p4[0]=reinterpret_cast<uint64_t>(p3)|7;p3[0]=reinterpret_cast<uint64_t>(p2)|7;
    p2[0]=reinterpret_cast<uint64_t>(p1)|7;p1[0]=0x100000|0x37;
    struct Tables {PhysicalFixture memory;uint64_t root;bool locked=false,available=true;int locks=0,unlocks=0;} tables;
    tables.memory.add(p4);tables.memory.add(p3);tables.memory.add(p2);tables.memory.add(p1);
    tables.root=reinterpret_cast<uint64_t>(p4)|0x1e;
    EPTSource::Operations operations{&tables,
        [](void *p,uintptr_t map) {auto &t=*static_cast<Tables *>(p);CHECK(map==0x4000 && !t.locked);if(!t.available)return false;t.locked=true;++t.locks;return true;},
        [](void *p,uintptr_t map) {auto &t=*static_cast<Tables *>(p);CHECK(map==0x4000 && t.locked);t.locked=false;++t.unlocks;},
        [](void *p,uintptr_t,uint64_t &root) {auto &t=*static_cast<Tables *>(p);CHECK(t.locked);root=t.root;return true;},
        [](void *p,uint64_t pa,void *out,size_t size) {auto &t=*static_cast<Tables *>(p);CHECK(t.locked);return PhysicalFixture::read(&t.memory,pa,out,size);}};
    GuestMemory::Registry memory;MapModel map;CHECK(memory.configure(map.operations()));
    EPTSource::Adapter adapter;CHECK(adapter.configure(operations,memory));auto source=adapter.sourceAccess();
    CHECK(NPTManager::configure(source));auto key=NPTManager::registerSpace(0xabc,tables.root);CHECK(key);
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);
        CHECK(adapter.bind(key,update,0x123,0x4000)==R::Success);
        CHECK(memory.wireRange(key,update,0,4096,3)==R::Success);
        CHECK(!NPTManager::retire(update));
    }
    // Proof starts at the retained pmap root, never at an arbitrary RAM page.
    CHECK(source.beginOwned(source.context,key));uint64_t entry=0;
    CHECK(!source.read(source.context,reinterpret_cast<uint64_t>(p3),&entry,8));
    CHECK(source.read(source.context,reinterpret_cast<uint64_t>(p4),&entry,8));
    CHECK(source.read(source.context,reinterpret_cast<uint64_t>(p3),&entry,8));
    CHECK(!source.read(source.context,reinterpret_cast<uint64_t>(p3)+1,&entry,8));
    source.end(source.context);CHECK(!tables.locked);
    CHECK(NPTManager::synchronize(key,0)==NPTManager::SyncResult::Unowned); // EPT execute exceeds the wire grant.
    CHECK(NPTManager::invalidateSpace(key));
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);p1[0]=0x100000|0x33;
    }
    CHECK(NPTManager::synchronize(key,0)==NPTManager::SyncResult::Synced);
    uint64_t word=0;CHECK(NPTManager::readGuestPhysical(key,0,&word,8) && word==0x5a5a5a5a5a5a5a5aULL);
    tables.available=false;CHECK(NPTManager::synchronize(key,0)==NPTManager::SyncResult::Unowned);
    tables.available=true;CHECK(NPTManager::invalidateSpace(key));
    tables.root|=0x40;CHECK(NPTManager::synchronize(key,0)==NPTManager::SyncResult::Unowned);
    tables.root=key.eptp();CHECK(NPTManager::invalidateSpace(key));
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);p4[0]=reinterpret_cast<uint64_t>(p4)|7;
    }
    CHECK(NPTManager::synchronize(key,0)==NPTManager::SyncResult::Unowned); // Cyclic source hierarchy.
    CHECK(!tables.locked && tables.locks==tables.unlocks);
    {
        auto update=NPTManager::beginUpdate(key);CHECK(update);
        CHECK(adapter.release(key,update)==R::Success && !map.references);
        CHECK(NPTManager::retire(update));
    }
    CHECK(NPTManager::shutdown());free(p4);free(p3);free(p2);free(p1);
}
static void lifecycle() {
    using namespace HostLifecycle;
    struct Hardware {bool enabled[4] {};int fail=2;bool failRestore=false;} hw;
    Controller host;
    CHECK(host.canDetachAdapter());
    CHECK(host.acquire(false)==Result::Unavailable);
    Adapter adapter{&hw,4,
        [](void *c,uint32_t cpu) {auto &h=*static_cast<Hardware *>(c);if(int(cpu)==h.fail)return false;h.enabled[cpu]=true;return true;},
        [](void *c,uint32_t cpu) {auto &h=*static_cast<Hardware *>(c);if(h.failRestore)return false;h.enabled[cpu]=false;return true;}};
    CHECK(host.configure(adapter)==Result::Success);
    CHECK(host.acquire(false)==Result::HardwareFailure);
    for(bool e:hw.enabled) CHECK(!e);
    hw.fail=-1;
    CHECK(host.acquire(false)==Result::Success && host.acquire(false)==Result::Success);
    CHECK(!host.canDetachAdapter());
    CHECK(host.detachAdapter()==Result::Conflict);
    CHECK(host.acquire(true)==Result::Conflict);
    auto run=host.beginRun(0);CHECK(run);
    CHECK(!host.beginRun(0));
    CHECK(host.release()==Result::Busy && host.suspend()==Result::Busy);
    CHECK(!host.beginRun(1) && host.acquire(false)==Result::Unavailable);
    run.reset();
    CHECK(host.suspend()==Result::Success && !host.beginRun(0));
    CHECK(host.resume()==Result::Success);
    CHECK(host.release()==Result::Success && host.release()==Result::Success);
    CHECK(host.canDetachAdapter());
    CHECK(host.release()==Result::Conflict);
    CHECK(host.acquire(true)==Result::Success && host.acquire(false)==Result::Conflict);
    hw.failRestore=true;CHECK(host.release()==Result::HardwareFailure);
    CHECK(!host.canDetachAdapter());
    CHECK(!host.beginRun(0));
    hw.failRestore=false;CHECK(host.release()==Result::Success);
    CHECK(host.acquire(false)==Result::Success && host.suspend()==Result::Success);
    CHECK(host.release()==Result::Success && host.acquire(false)==Result::Unavailable);
    CHECK(host.resume()==Result::Success && host.resume()==Result::Success);
    CHECK(host.acquire(false)==Result::Success);
    auto closing=host.beginRun(1);CHECK(closing && host.suspend()==Result::Busy);closing.reset();
    CHECK(host.release()==Result::Success && host.acquire(false)==Result::Unavailable);
    CHECK(host.resume()==Result::Success);
    CHECK(host.idle() && host.detachAdapter()==Result::Success && !host.idle());
    CHECK(host.acquire(false)==Result::Unavailable);
}
static void transitions() {
    CHECK(SoftVMCS::prepareResources(1,65));
    auto v=SoftVMCS::getOrCreateByPhysAddr(0x1000);CHECK(v);
    AppleHVGPRFrame frame{};
    using namespace VMCBTranslator;
    CHECK(syncVMCStoVMCB(v.get(),&frame)==EntryResult::UnsupportedControls);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_CPU_BASED,CPU_BASED_UNCOND_IO_EXIT);
    CHECK(syncVMCStoVMCB(v.get(),&frame)==EntryResult::MissingNPT);
    CHECK(v->vmcbVA->control.np_enable==1 && !v->vmcbVA->control.n_cr3 && v->vmcbVA->control.tlb_control==1);
    CHECK((v->vmcbVA->control.intercept_ops1&15)==15 && (v->vmcbVA->control.vintr&SVM_VINTR_INTR_MASKING));
    SoftVMCS::writeField(v.get(),VMCS_CTRL_CPU_BASED,CPU_BASED_UNCOND_IO_EXIT|CPU_BASED_TPR_SHADOW);
    CHECK(syncVMCStoVMCB(v.get(),&frame)==EntryResult::UnsupportedControls);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_CPU_BASED,CPU_BASED_UNCOND_IO_EXIT);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_VMENTRY_IRQ_INFO,0x80000420); // Unsupported software interrupt.
    CHECK(syncVMCStoVMCB(v.get(),&frame)==EntryResult::InvalidEvent);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_VMENTRY_IRQ_INFO,0x80000020);
    CHECK(syncVMCStoVMCB(v.get(),&frame)==EntryResult::MissingNPT);
    uint64_t pending=0;SoftVMCS::readField(v.get(),VMCS_CTRL_VMENTRY_IRQ_INFO,pending);CHECK(pending==0x80000020);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_INVALID;
    frame.rax=0x123;bool retry=false;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::FailedEntry && frame.rax==0x123);
    SoftVMCS::readField(v.get(),VMCS_CTRL_VMENTRY_IRQ_INFO,pending);CHECK(pending==0x80000020);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_HLT;
    v->vmcbVA->control.next_rip=v->vmcbVA->state.rip+1;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::GuestExit);
    SoftVMCS::readField(v.get(),VMCS_CTRL_VMENTRY_IRQ_INFO,pending);CHECK(!pending);
    v->vmcbVA->control.exit_code=0xabcdef;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    uint64_t reason=0;SoftVMCS::readField(v.get(),VMCS_RO_EXIT_REASON,reason);CHECK(reason==0xffffffff);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_IOIO;
    v->vmcbVA->control.exit_info_1=0x00800010;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    v->vmcbVA->control.num_bytes_fetched=2;
    v->vmcbVA->control.next_rip=0;
    v->vmcbVA->control.guest_inst_bytes[0]=0xe6;v->vmcbVA->control.guest_inst_bytes[1]=0x80;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::GuestExit);
    uint64_t qualification=0;SoftVMCS::readField(v.get(),VMCS_RO_EXIT_QUALIFIC,qualification);
    CHECK(qualification==0x00800040);
    v->vmcbVA->state.cs.attrib|=1U<<10;
    v->vmcbVA->control.guest_inst_bytes[0]=0xe7;v->vmcbVA->control.exit_info_1=0x00800040;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::GuestExit);
    v->vmcbVA->control.exit_info_1=0x00800020;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    v->vmcbVA->control.guest_inst_bytes[0]=0x66;v->vmcbVA->control.guest_inst_bytes[1]=0xe7;
    v->vmcbVA->control.guest_inst_bytes[2]=0x80;v->vmcbVA->control.num_bytes_fetched=3;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::GuestExit);
    v->vmcbVA->control.guest_inst_bytes[0]=0xe6;v->vmcbVA->control.guest_inst_bytes[1]=0x80;
    v->vmcbVA->control.num_bytes_fetched=2;v->vmcbVA->control.exit_info_1=0x00800010;
    v->vmcbVA->control.guest_inst_bytes[1]=0x81;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    v->vmcbVA->control.guest_inst_bytes[1]=0x80;
    v->vmcbVA->control.exit_info_1^=1;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    v->vmcbVA->control.exit_info_1^=1;
    v->vmcbVA->control.next_rip=v->vmcbVA->state.rip+1;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    v->vmcbVA->control.next_rip=0;
    v->vmcbVA->control.exit_info_1|=0x20;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    GuestMSRs msrs;msrs.count=33;
    CHECK(syncVMCStoVMCB(v.get(),&frame,&msrs)==EntryResult::InvalidMSRs);
    msrs.count=1;msrs.entries[0].index=MSR_IA32_LSTAR;msrs.entries[0].data=0x0000800000000000ULL;
    CHECK(syncVMCStoVMCB(v.get(),&frame,&msrs)==EntryResult::InvalidMSRs);
    CHECK(syncVMCBtoVMCS(v.get(),&frame,&msrs,retry)==ExitResult::InvalidArgument);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_CPUID;v->vmcbVA->control.num_bytes_fetched=0;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::UnsupportedExit);
    SoftVMCS::writeField(v.get(),VMCS_RO_IDT_VECTOR_INFO,0x8000030e);
    SoftVMCS::writeField(v.get(),VMCS_RO_IO_RCX,0xbad);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_INVALID;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::FailedEntry);
    uint64_t stale=1;SoftVMCS::readField(v.get(),VMCS_RO_IDT_VECTOR_INFO,stale);CHECK(!stale);
    SoftVMCS::readField(v.get(),VMCS_RO_IO_RCX,stale);CHECK(!stale);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_INTR;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::HostEvent && !retry);
    SoftVMCS::readField(v.get(),VMCS_RO_EXIT_REASON,stale);CHECK(stale==0xffffffff);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_PIN_BASED,PIN_BASED_INTR_EXIT);
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::GuestExit);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_NMI;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::HostEvent);
    v->vmcbVA->control.exit_code=SVM_VMEXIT_SMI;
    CHECK(syncVMCBtoVMCS(v.get(),&frame,nullptr,retry)==ExitResult::HostEvent);
    msrs.count=2;msrs.entries[0].index=msrs.entries[1].index=MSR_IA32_STAR;
    CHECK(syncVMCStoVMCB(v.get(),&frame,&msrs)==EntryResult::InvalidMSRs);
    uint64_t event=0;CHECK(!Architecture::toSVMEvent(0x80000808,1,event));
    CHECK(Architecture::toSVMEvent(0x20,0,event) && !event);
    CHECK(!SoftVMCS::writeSynthesizedMSR(MSR_IA32_SPEC_CTRL,1));
    CHECK(!SoftVMCS::writeSynthesizedMSR(MSR_IA32_VMX_BASIC,1));
    v.reset();CHECK(SoftVMCS::shutdown());CHECK(NPTManager::shutdown());
}
int main() {
    failureInjection();ownership();isolation();guestOwnership();wireRanges();shadowAliasAndLargeLeaf();pinnedSource();lifecycle();transitions();vcpuLifetimes();trapOwnership();
    CHECK(Memory::outstanding()==0);
    puts("Allocation rollback, lifetime, concurrency, isolation, lifecycle and transition tests passed.");
}
