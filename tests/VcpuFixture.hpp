#pragma once
#include <src/VcpuLifecycle.hpp>
#include <memory>
struct VcpuFixture {
    alignas(8) uint8_t object[0x398] {},vm[0x288] {};
    uint8_t *rw {},*ro {},*cs {};
    int references[3] {},retains[3] {};
    uintptr_t thread=1;
    int failRetain=-1,failInspect=-1;
    PhysicalFixture tables;
    NPTManager::SpaceKey key;
    std::unique_ptr<VcpuLifecycle::Registry> registry {new VcpuLifecycle::Registry};
    template<class T> void put(size_t offset,T value) {memcpy(object+offset,&value,sizeof(value));}
    uint64_t address() const {return reinterpret_cast<uint64_t>(object);}
    static uint8_t *page() {void *p=nullptr;CHECK(!posix_memalign(&p,4096,8192));memset(p,0,8192);return static_cast<uint8_t *>(p);}
    VcpuFixture() {
        rw=page();ro=page();cs=page();
        tables.ownedOwner=0xabc;CHECK(NPTManager::prepareResources(4) && tables.install());
        key=NPTManager::registerSpace(0xabc,0x101e);CHECK(key);
        put(0x28,reinterpret_cast<uint64_t>(vm));put(0x30,uint64_t(0xabc));
        put(0x38,reinterpret_cast<uint64_t>(rw)+4096);put(0x40,reinterpret_cast<uint64_t>(ro)+19);
        uintptr_t descriptor=1;memcpy(vm+0x280,&descriptor,8);descriptor=2;memcpy(vm+0x270,&descriptor,8);
        VcpuLifecycle::Operations ops;
        ops.context=this;ops.memory={this,[](void *p,uint64_t address,void *out,size_t bytes) {
            auto &m=*static_cast<VcpuFixture *>(p);
            PhysicalFixture::Span spans[]={{m.address(),sizeof(m.object)},{reinterpret_cast<uint64_t>(m.vm),sizeof(m.vm)},
                {reinterpret_cast<uint64_t>(m.rw),8192},{reinterpret_cast<uint64_t>(m.ro),8192},{reinterpret_cast<uint64_t>(m.cs),8192}};
            for(auto s:spans) if(address>=s.address && address-s.address<=s.size && bytes<=s.size-(address-s.address)) {
                memcpy(out,reinterpret_cast<void *>(address),bytes);return true;
            }
            return false;
        },nullptr,nullptr};
        ops.thread=[](void *p) {return static_cast<VcpuFixture *>(p)->thread;};
        ops.retain=[](void *p,uintptr_t descriptor) {
            auto &m=*static_cast<VcpuFixture *>(p);CHECK(descriptor>=1 && descriptor<=3);
            auto i=descriptor-1;if(m.failRetain==int(i)) return false;++m.references[i];++m.retains[i];return true;
        };
        ops.release=[](void *p,uintptr_t descriptor) {
            auto &m=*static_cast<VcpuFixture *>(p);CHECK(descriptor>=1 && descriptor<=3 && m.references[descriptor-1]>0);--m.references[descriptor-1];
        };
        ops.inspect=[](void *p,uintptr_t descriptor,VcpuLifecycle::Buffer &out) {
            auto &m=*static_cast<VcpuFixture *>(p);CHECK(descriptor>=1 && descriptor<=3 && m.references[descriptor-1]>0);
            auto i=descriptor-1;if(m.failInspect==int(i)) return false;
            out={reinterpret_cast<uint64_t>(i==0?m.rw:i==1?m.ro:m.cs),i==2?4096U:8192U};return true;
        };
        CHECK(registry->configure(ops) && !registry->configure(ops));
    }
    void allocateCS(uint64_t pa=0x5000) {put(0x88,uint64_t(3));put(0x90,pa);uint32_t revision=1;memcpy(cs,&revision,4);}
    void freeObject() {memset(object,0xa5,sizeof(object));}
    ~VcpuFixture() {
        CHECK(registry->empty());for(auto count:references) CHECK(!count);
        CHECK(NPTManager::releaseSpace(key) && NPTManager::shutdown());
        free(rw);free(ro);free(cs);
    }
};
static void vcpuLifetimes() {
    CHECK(SoftVMCS::prepareResources(2,8));
    {
        VcpuFixture m;
        auto source=NPTManager::retainSpace(m.key);CHECK(source && !NPTManager::hasNoOwners(m.key));
        CHECK(!NPTManager::releaseSpace(m.key) && !NPTManager::shutdown());
        auto update=NPTManager::beginUpdate(m.key);CHECK(update);
        CHECK(!NPTManager::retainSpace(m.key) && !NPTManager::retire(update));
        update.reset();source.reset();CHECK(NPTManager::hasNoOwners(m.key));
        CHECK(!m.registry->acquirePhysical(0x5000));
        auto init=m.registry->begin(m.address(),m.key);CHECK(init);
        CHECK(!m.registry->begin(m.address(),m.key) && !m.registry->acquire(m.address()) && !m.registry->retire(m.address()));
        CHECK(!NPTManager::releaseSpace(m.key));
        m.allocateCS();m.thread=2;CHECK(!m.registry->acquirePhysical(0x5000));m.thread=1;
        CHECK(!m.registry->acquirePhysical(0x5001) && !m.registry->acquirePhysical(0x6000));
        uint32_t invalid=2;memcpy(m.cs,&invalid,4);CHECK(!m.registry->acquirePhysical(0x5000));
        CHECK(m.references[2]==1);invalid=1;memcpy(m.cs,&invalid,4);
        auto region=m.registry->acquirePhysical(0x5000);CHECK(region && region.owner()==m.address());
        uint64_t epoch=region.lifetime();CHECK(epoch && region.space().lifetime()==m.key.lifetime());
        uint32_t revision=0;CHECK(region.readRevision(0x5000,&revision,4) && revision==1);
        CHECK(!region.readRevision(0x5004,&revision,4) && !region.readRevision(0x5000,&revision,3));
        CHECK(region.owns(reinterpret_cast<uint64_t>(m.rw)+4096,4096,true));
        CHECK(!region.owns(reinterpret_cast<uint64_t>(m.rw)+4095,2,true));
        CHECK(!region.owns(reinterpret_cast<uint64_t>(m.rw)+8191,2,true));
        CHECK(region.owns(reinterpret_cast<uint64_t>(m.ro)+19,1) && !region.owns(reinterpret_cast<uint64_t>(m.ro)+19,1,true));
        CHECK(region.owns(m.address(),0x394) && !region.owns(m.address(),1,true));
        CHECK(region.owns(reinterpret_cast<uint64_t>(m.cs),4096) && !region.owns(reinterpret_cast<uint64_t>(m.cs),1,true));
        CHECK(!region.owns(UINT64_MAX,2));
        region.reset();CHECK(m.registry->acquirePhysical(0x5000) && m.retains[2]==1);
        CHECK(init.finish(true) && !init.finish(true));
        auto core=SoftVMCS::findByPhysAddr(0x5000);CHECK(core && core->appleOwner==m.address() && core->appleLifetime==epoch);
        CHECK(SoftVMCS::setActiveVMCS(0,core));core.reset();
        update=NPTManager::beginUpdate(m.key);CHECK(update && !NPTManager::retire(update));update.reset();
        auto held=m.registry->acquire(m.address());CHECK(held);
        CHECK(!m.registry->retire(m.address()) && !m.registry->acquire(m.address()) && !m.registry->acquirePhysical(0x5000));
        held.reset();CHECK(!m.registry->retire(m.address())); // active VMCS binding
        CHECK(SoftVMCS::clearActiveVMCS(0));
        core=SoftVMCS::findByPhysAddr(0x5000);CHECK(core && !m.registry->retire(m.address()));core.reset();
        auto retirement=m.registry->retire(m.address());CHECK(retirement);
        CHECK(NPTManager::hasNoOwners(m.key) && !SoftVMCS::findByPhysAddr(0x5000));
        CHECK(!m.registry->retire(m.address()) && !m.registry->untracked(m.address()));
        for(auto count:m.references) CHECK(count==1);
        m.freeObject();retirement.reset();CHECK(m.registry->empty());
        // Same address and same physical page get a fresh catalog epoch.
        m.put(0x28,reinterpret_cast<uint64_t>(m.vm));m.put(0x30,uint64_t(0xabc));
        m.put(0x38,reinterpret_cast<uint64_t>(m.rw)+4096);m.put(0x40,reinterpret_cast<uint64_t>(m.ro)+19);m.allocateCS();
        auto again=m.registry->begin(m.address(),m.key);CHECK(again);
        region=m.registry->acquirePhysical(0x5000);CHECK(region && region.lifetime()>epoch);region.reset();
        CHECK(again.finish(true));retirement=m.registry->retire(m.address());CHECK(retirement);retirement.reset();
    }
    for(int failure=0;failure<5;++failure) {
        VcpuFixture m;
        if(failure<2) m.failRetain=failure;
        else if(failure<4) m.failInspect=failure-2;
        auto init=m.registry->begin(m.address(),m.key);
        if(failure==4) {CHECK(init && !init.finish(true));}else CHECK(!init);
        CHECK(!m.registry->untracked(m.address()) && !NPTManager::releaseSpace(m.key));
        auto retirement=m.registry->retire(m.address());CHECK(retirement);m.freeObject();retirement.reset();
    }
    {
        VcpuFixture m;auto init=m.registry->begin(m.address(),m.key);CHECK(init);m.allocateCS();
        auto unrelated=SoftVMCS::getOrCreateByPhysAddr(0x5000);CHECK(unrelated);unrelated.reset();
        CHECK(!m.registry->acquirePhysical(0x5000));CHECK(!init.finish(false));
        auto retirement=m.registry->retire(m.address());CHECK(retirement);retirement.reset();
        CHECK(SoftVMCS::releaseByPhysAddr(0x5000));
    }
    {
        VcpuFixture m;auto init=m.registry->begin(m.address(),m.key);CHECK(init);m.allocateCS();
        {auto region=m.registry->acquirePhysical(0x5000);CHECK(region);}
        CHECK(init.finish(true));
        std::atomic<unsigned> reads {0};std::atomic<bool> stop {false};std::vector<std::thread> workers;
        for(unsigned i=0;i<12;++i) workers.emplace_back([&] {
            while(!stop.load()) {
                auto region=m.registry->acquire(m.address());if(!region) {std::this_thread::yield();continue;}
                uint32_t revision=0;CHECK(region.readRevision(0x5000,&revision,4) && revision==1);
                CHECK(region.owns(reinterpret_cast<uint64_t>(m.rw)+4096,4096,true));++reads;
            }
        });
        while(reads.load()<1000) std::this_thread::yield();
        auto retirement=m.registry->retire(m.address());
        while(!retirement) {std::this_thread::yield();retirement=m.registry->retire(m.address());}
        m.freeObject();stop=true;for(auto &worker:workers) worker.join();retirement.reset();
    }
    CHECK(SoftVMCS::shutdown());
}
