#include <src/AppleHVABI.hpp>
#include <src/SVMCPU.hpp>
#include <src/FaultMemory.hpp>
#include <src/XNUPlatform.hpp>
#include <src/SVMTransition.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <new>
bool amdhvDebugEnabled=false;
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"CHECK %s at %d\n",#x,__LINE__);abort();}} while(0)
struct Hardware {
    struct CPU {uint64_t efer=0x901,hsave=0x5000,pat=0x0007040600070406ULL,vmcr=0;} cpus[12];
    uint32_t cpu=0;int writeCount=0,failWrite=-1;bool failAfterWrite=false,online=true;uint32_t asids=8;
    SVMCPU::Operations ops(uint32_t count=4) {
        CHECK(count<=12);SVMCPU::Operations out;out.context=this;out.cpus=count;out.requiredASIDs=8;
        out.currentCPU=[](void *p) {return static_cast<Hardware *>(p)->cpu;};
        out.onCPU=[](void *p,uint32_t cpu,void(*fn)(void *),void *arg) {
            auto &h=*static_cast<Hardware *>(p);if(!h.online) return false;h.cpu=cpu;fn(arg);return true;
        };
        out.features=[](void *p,SVMCPU::Features &f) {auto &h=*static_cast<Hardware *>(p);f={h.asids,48,true,true,true};return true;};
        out.read=[](void *p,uint32_t index,uint64_t &value) {
            auto &h=*static_cast<Hardware *>(p);auto &c=h.cpus[h.cpu];
            switch(index) {case MSR_AMD_EFER:value=c.efer;break;case MSR_AMD_VM_HSAVE_PA:value=c.hsave;break;
                case MSR_AMD_VM_CR:value=c.vmcr;break;case MSR_IA32_PAT:value=c.pat;break;default:return false;}return true;
        };
        out.write=[](void *p,uint32_t index,uint64_t value) {
            auto &h=*static_cast<Hardware *>(p);auto &c=h.cpus[h.cpu];bool fail=h.writeCount++==h.failWrite;
            if(fail && !h.failAfterWrite) return false;
            switch(index) {case MSR_AMD_EFER:c.efer=value;break;case MSR_AMD_VM_HSAVE_PA:c.hsave=value;break;default:return false;}
            return !fail;
        };return out;
    }
};
static void cpuTests() {
    const uint64_t pages[]={0x10000,0x11000,0x12000,0x13000};
    {
        Hardware h;h.failWrite=1;SVMCPU::Driver driver;CHECK(driver.configure(h.ops(),pages,4));
        CHECK(!driver.enable(0) && driver.ownsCPU(0) && !driver.readyCPU(0));CHECK(driver.restore(0) && !driver.ownsCPU(0));
        h.failWrite=-1;CHECK(driver.enable(0) && driver.readyCPU(0));h.online=false;
        CHECK(!driver.restore(0) && !driver.readyCPU(0) && driver.ownsCPU(0));h.online=true;CHECK(driver.restore(0));
    }
    {
        Hardware h;SVMCPU::Driver driver;uint64_t twelve[12];for(unsigned i=0;i<12;++i) twelve[i]=0x10000+i*4096;
        CHECK(driver.configure(h.ops(12),twelve,12));HostLifecycle::Controller host;CHECK(host.configure(driver.adapter())==HostLifecycle::Result::Success);
        CHECK(host.acquire(false)==HostLifecycle::Result::Success);
        for(unsigned i=0;i<12;++i) CHECK(driver.readyCPU(i) && h.cpus[i].hsave==twelve[i]);
        CHECK(host.suspend()==HostLifecycle::Result::Success);
        for(unsigned i=0;i<12;++i) CHECK(!driver.readyCPU(i) && h.cpus[i].hsave==0x5000 && h.cpus[i].efer==0x901);
        CHECK(host.resume()==HostLifecycle::Result::Success && host.release()==HostLifecycle::Result::Success);
    }
    for(int fail=0;fail<8;++fail) for(unsigned after=0;after<2;++after) {
        Hardware h;h.failWrite=fail;h.failAfterWrite=after;
        SVMCPU::Driver driver;CHECK(driver.configure(h.ops(),pages,4));
        HostLifecycle::Controller host;CHECK(host.configure(driver.adapter())==HostLifecycle::Result::Success);
        CHECK(host.acquire(false)==HostLifecycle::Result::HardwareFailure);
        for(unsigned i=0;i<4;++i) CHECK(h.cpus[i].efer==0x901 && h.cpus[i].hsave==0x5000 && !driver.ownsCPU(i));
    }
    Hardware h;SVMCPU::Driver driver;CHECK(driver.configure(h.ops(),pages,4));
    HostLifecycle::Controller host;CHECK(host.configure(driver.adapter())==HostLifecycle::Result::Success);
    h.cpus[1].efer|=EFER_SVME;
    CHECK(host.acquire(false)==HostLifecycle::Result::HardwareFailure);
    CHECK(h.cpus[0].efer==0x901 && (h.cpus[1].efer&EFER_SVME));
    h.cpus[1].efer=0x901;h.cpus[1].vmcr=VM_CR_SVMDIS;
    CHECK(host.acquire(false)==HostLifecycle::Result::HardwareFailure);
    h.cpus[1].vmcr=0;h.asids=2;
    CHECK(host.acquire(false)==HostLifecycle::Result::HardwareFailure);
    h.asids=8;h.online=false;
    CHECK(host.acquire(false)==HostLifecycle::Result::HardwareFailure);
    h.online=true;
    // The offline rendezvous prevented rollback acknowledgement; retry release
    // is needed only if the controller retained a faulted acquisition.
    CHECK(host.acquire(false)==HostLifecycle::Result::Unavailable);
    CHECK(host.recover()==HostLifecycle::Result::Success);
    CHECK(host.acquire(false)==HostLifecycle::Result::Success);
    CHECK(host.release()==HostLifecycle::Result::Success);
    // A fresh controller remains able to exercise the reversible hardware state.
    Hardware live;SVMCPU::Driver driver2;CHECK(driver2.configure(live.ops(),pages,4));
    HostLifecycle::Controller healthy;CHECK(healthy.configure(driver2.adapter())==HostLifecycle::Result::Success);
    CHECK(healthy.acquire(false)==HostLifecycle::Result::Success);
    for(unsigned i=0;i<4;++i) CHECK(driver2.readyCPU(i) && live.cpus[i].hsave==pages[i] && (live.cpus[i].efer&EFER_SVME));
    CHECK(!driver2.readyCPU(128));
    CHECK(healthy.suspend()==HostLifecycle::Result::Success);
    for(unsigned i=0;i<4;++i) CHECK(!driver2.readyCPU(i));
    for(auto &cpu:live.cpus) CHECK(cpu.hsave==0x5000 && cpu.efer==0x901);
    CHECK(healthy.resume()==HostLifecycle::Result::Success);
    CHECK(healthy.release()==HostLifecycle::Result::Success);
}
struct View {
    uint8_t *base;size_t size;
    static bool read(void *p,uint64_t a,void *out,size_t n) {
        auto &v=*static_cast<View *>(p);auto b=reinterpret_cast<uint64_t>(v.base);
        if(a<b || a-b>v.size || n>v.size-(a-b)) return false;
        memcpy(out,v.base+(a-b),n);return true;
    }
};
static void abiTests() {
    alignas(4096) uint8_t object[4096] {};
    View view{object,sizeof(object)};MemoryAccess::Access memory{&view,View::read,nullptr,nullptr};
    auto put=[&](size_t off,uint64_t value,size_t n=8) {memcpy(object+off,&value,n);};
    put(0x24,3,4);put(0x28,0x1000);put(0x30,0x2000);put(0x38,0x3000);put(0x40,0x4000);
    put(0x90,0x5000);put(0x120,7,4);put(0x180,0xffff0ff0);put(0x385,1,1);
    AppleHVABI::Vcpu v;
    uint64_t address=reinterpret_cast<uint64_t>(object);
    CHECK(AppleHVABI::readVcpu(memory,address+0x385,0x3c80,v));
    CHECK(v.id==3 && v.vmcsPA==0x5000 && v.cpu==7 && v.launched && v.guestDR6==0xffff0ff0);
    put(0x68,0xbad000);CHECK(AppleHVABI::readVcpu(memory,address+0x385,0x3c80,v) && v.vmcsPA==0x5000);
    CHECK(!AppleHVABI::readVcpu(memory,address+0x385,0x4c80,v) && !v.address);
    put(0x90,0x5001);CHECK(!AppleHVABI::readVcpu(memory,address+0x385,0x3c80,v));
    memset(object,0,sizeof(object));put(0x10,0x1000);put(0x18,0x2000);put(0x20,0x3000);put(0x30,0x4000);
    AppleHVABI::Space s;CHECK(AppleHVABI::readSpace(memory,address,s) && s.eptp==0x201e);
    put(0x18,0x201e);CHECK(AppleHVABI::readSpace(memory,address,s) && s.rootPA==0x2000 && s.eptp==0x201e);
    put(0x18,0x205e);CHECK(!AppleHVABI::readSpace(memory,address,s));
    put(0x18,0x201e);put(0x28,1,1);CHECK(!AppleHVABI::readSpace(memory,address,s));
}
static bool interrupts=true;
static FaultMemory::Context probeContext() {
    return {nullptr,[](void *) {return 0U;},[](void *) {return uintptr_t(1);},
        [](void *) {bool previous=interrupts;interrupts=false;return previous;},
        [](void *,bool previous) {interrupts=previous;},[](void *) {return true;},
        [](void *,uint64_t,size_t,bool) {return true;}};
}
static void faultSignal(int number,siginfo_t *,void *argument) {
    auto context=static_cast<ucontext_t *>(argument);
    XNUSavedState64 state{};state.flavor=15;state.cs=8;state.trapno=14;
    state.rip=context->uc_mcontext->__ss.__rip;
    if(!TrapRecovery::registry().handle(0,1,&state)) _exit(128+number);
    context->uc_mcontext->__ss.__rip=state.rip;
}
static void recoveryTests() {
    TrapRecovery::Registry registry;
    TrapRecovery::Site sites[]={{0x1000,0x2000,1U<<14}};
    TrapRecovery::Record record{1,sites,1,0};
    auto scope=registry.arm(0,record);CHECK(scope && !registry.arm(0,record));
    XNUSavedState64 state{};state.flavor=15;state.cs=8;state.trapno=14;state.rip=0x1000;
    CHECK(!registry.handle(0,2,&state) && state.rip==0x1000);
    state.cs=3;CHECK(!registry.handle(0,1,&state));state.cs=8;
    CHECK(registry.handle(0,1,&state) && state.rip==0x2000);
    CHECK(!registry.arm(0,record) && record.faults==1);
    state.rip=0x1000;CHECK(!registry.handle(0,1,&state));scope.reset();CHECK(!registry.active(0));
    struct sigaction handler{},previousSEGV{},previousBUS{};
    handler.sa_sigaction=faultSignal;handler.sa_flags=SA_SIGINFO;
    CHECK(!sigemptyset(&handler.sa_mask));CHECK(!sigaction(SIGSEGV,&handler,&previousSEGV));CHECK(!sigaction(SIGBUS,&handler,&previousBUS));
    void *guard=mmap(nullptr,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);CHECK(guard!=MAP_FAILED);
    FaultMemory::Adapter adapter(probeContext());
    uint8_t value=0x55,output=0;
    CHECK(adapter.read(reinterpret_cast<uint64_t>(&value),&output,1) && output==0x55 && interrupts);
    CHECK(!adapter.read(reinterpret_cast<uint64_t>(guard),&output,1) && interrupts);
    CHECK(!adapter.write(reinterpret_cast<uint64_t>(guard),&value,1) && interrupts);
    uint64_t word=0x12345678;
    CHECK(!adapter.write(reinterpret_cast<uint64_t>(guard),&word,8) && interrupts);
    CHECK(!adapter.write(reinterpret_cast<uint64_t>(&word)+1,&word,8));
    uint64_t other=0;CHECK(adapter.write(reinterpret_cast<uint64_t>(&other),&word,8) && other==word);
    CHECK(!munmap(guard,4096));CHECK(!sigaction(SIGSEGV,&previousSEGV,nullptr));CHECK(!sigaction(SIGBUS,&previousBUS,nullptr));
}
static constexpr uintptr_t testMap=0xffff800000100000ULL;
static uint64_t physicalBase=0xffff800001000000ULL,physicalLimit=physicalBase+0x10000;
static int mapReferences=0,nativeWires=0,nativeUnwires=0;static uint16_t mapShift=12;
static int preemptible=1,interruptContext=0,pmapLocks=0;
static uint64_t stackBytes=16384,stackAddress=0xffff800000400000ULL;
static void nativeMapTests() {
    CHECK(!XNUPlatform::memoryOperations().wire);
    CHECK(!XNUPlatform::faultOperations([](void *) {return true;}).cpu);
    XNUPlatform::Services services;
    services.cpu=[] {return 0U;};services.thread=[] {return uintptr_t(1);};
    services.rendezvous=[](void(*action)(void *),void *arg) {action(arg);};
    services.readMSR=[](uint32_t,uint64_t *) {return 1;};services.writeMSR=[](uint32_t,uint64_t) {return 1;};
    services.copyNoFault=[](uintptr_t address,uintptr_t out,size_t size) -> size_t {
        if(address==testMap+0x100 && size==8) {memcpy(reinterpret_cast<void *>(out),&stackBytes,8);return 8;}
        if(address==testMap+0x44 && size==2) {memcpy(reinterpret_cast<void *>(out),&mapShift,2);return 2;}
        if(address==testMap+0x60 && size==8) {uint64_t root=0x501e;memcpy(reinterpret_cast<void *>(out),&root,8);return 8;}
        if(address<physicalBase || address-physicalBase>0x10000 || size>0x10000-(address-physicalBase)) return 0;
        memset(reinterpret_cast<void *>(out),0x6b,size);return size;
    };
    services.maps.reference=[](uintptr_t map) {CHECK(map==testMap);++mapReferences;};
    services.maps.deallocate=[](uintptr_t map) {CHECK(map==testMap);--mapReferences;};
    services.maps.wireAndExtract=[](uintptr_t map,uint64_t address,uint32_t permissions,int user,uint32_t *page) {
        CHECK(map==testMap && address==0x1000 && permissions==3 && !user);++nativeWires;*page=5;return 0;
    };
    services.maps.unwire=[](uintptr_t map,uint64_t first,uint64_t end,int user) {
        CHECK(map==testMap && first==0x1000 && end==0x2000 && !user);++nativeUnwires;return 0;
    };
    services.maps.validPage=[](uint32_t page) {return page==5?1:0;};
    services.maps.physmapBase=&physicalBase;services.maps.physmapMax=&physicalLimit;
    services.maps.tryPmapShared=[](uintptr_t pmap) {CHECK(pmap==testMap);++pmapLocks;return 1;};
    services.maps.unlockPmapShared=[](uintptr_t pmap) {CHECK(pmap==testMap);--pmapLocks;};
    services.maps.preemptionEnabled=[] {return preemptible;};services.maps.atInterruptContext=[] {return interruptContext;};
    services.traps.setInterrupts=[](int enabled) {bool previous=interrupts;interrupts=enabled!=0;return int(previous);};
    services.traps.threadStack=[](uintptr_t thread) {CHECK(thread==1);return uintptr_t(stackAddress);};
    services.traps.stackBytes=reinterpret_cast<const uint64_t *>(testMap+0x100);
    CHECK(XNUPlatform::configure(services) && !XNUPlatform::configure(services));
    auto faults=XNUPlatform::faultOperations([](void *p) {return *static_cast<bool *>(p);},&interrupts);
    CHECK(faults.cpu && faults.probeInstalled(faults.context) && faults.thread(faults.context)==1);
    CHECK(faults.disableInterrupts(faults.context) && !interrupts);faults.restoreInterrupts(faults.context,true);CHECK(interrupts);
    VcpuLifecycle::Buffer stack;
    CHECK(XNUPlatform::currentStack(1,stack) && stack.address==stackAddress && stack.bytes==stackBytes);
    CHECK(!XNUPlatform::currentStack(2,stack) && !stack.address);
    stackBytes=8193;CHECK(!XNUPlatform::currentStack(1,stack));stackBytes=16384;
    stackAddress=0;CHECK(!XNUPlatform::currentStack(1,stack));stackAddress=0xffff800000400000ULL;
    auto ops=XNUPlatform::memoryOperations();CHECK(ops.wire && ops.readPhysical);
    ops.retainMap(nullptr,testMap);CHECK(mapReferences==1);
    uint64_t pa=0;CHECK(!ops.wire(nullptr,testMap,0x1000,3,pa) && pa==0x5000 && nativeWires==1);
    mapShift=14;CHECK(ops.wire(nullptr,testMap,0x1000,3,pa) && !pa && nativeWires==1);mapShift=12;
    CHECK(ops.wire(nullptr,testMap,0x1001,3,pa) && nativeWires==1);
    CHECK(!ops.unwire(nullptr,testMap,0x1000,0x2000) && nativeUnwires==1);
    uint64_t value=0;CHECK(ops.readPhysical(nullptr,0x5000,&value,8) && value==0x6b6b6b6b6b6b6b6bULL);
    CHECK(!ops.readPhysical(nullptr,0x6000,&value,8));
    CHECK(!ops.readPhysical(nullptr,0x5ffc,&value,8));
    CHECK(!ops.readPhysical(nullptr,0x10000,&value,8));
    CHECK(!ops.readPhysical(nullptr,UINT64_MAX-3,&value,8));
    ops.releaseMap(nullptr,testMap);CHECK(!mapReferences);
    auto source=XNUPlatform::sourceOperations();CHECK(source.tryLock && source.root);
    preemptible=0;CHECK(!source.tryLock(nullptr,testMap) && !pmapLocks);preemptible=1;
    interruptContext=1;CHECK(!source.tryLock(nullptr,testMap) && !pmapLocks);interruptContext=0;
    CHECK(!source.tryLock(nullptr,testMap+1) && !pmapLocks);
    CHECK(source.tryLock(nullptr,testMap) && pmapLocks==1);
    uint64_t root=0;CHECK(source.root(nullptr,testMap,root) && root==0x501e);
    source.unlock(nullptr,testMap);CHECK(!pmapLocks);
}
static void transitionPreparation() {
    void *storage=nullptr,*guest=nullptr;
    CHECK(!posix_memalign(&storage,4096,sizeof(SVMTransition::Context)));
    CHECK(!posix_memalign(&guest,4096,4096));
    auto context=new(storage) SVMTransition::Context;
    auto vmcb=static_cast<VMCB *>(guest);AppleHVGPRFrame frame{};
    CHECK(SVMTransition::initialize(*context,vmcb,0x1000,0x2000,&frame,8) && context->ready);
    CHECK(context->privateIDTR.limit==4095 && context->privateIDTR.base==reinterpret_cast<uint64_t>(context->idt));
    for(unsigned i=0;i<256;++i) {
        auto &gate=context->idt[i];
        uint64_t address=gate.low|(uint64_t(gate.middle)<<16)|(uint64_t(gate.high)<<32);
        CHECK(address==reinterpret_cast<uint64_t>(amdhv_svm_trap_offsets)+int64_t(amdhv_svm_trap_offsets[i]));
        CHECK(gate.selector==8 && gate.type==0x8e && !gate.ist && !gate.reserved);
    }
    CHECK(SVMTransition::run(*context)==SVMTransition::Result::Unavailable);
    CHECK(!SVMTransition::initialize(*context,vmcb,0x1000,0x1000,&frame,8) && !context->ready);
    CHECK(!SVMTransition::initialize(*context,vmcb,0x1001,0x2000,&frame,8));
    CHECK(!SVMTransition::initialize(*context,vmcb,0x1000,0x2000,&frame,3));
    CHECK(!SVMTransition::initialize(*context,reinterpret_cast<VMCB *>(context->idt),0x1000,0x2000,&frame,8));
    CHECK(!SVMTransition::initialize(*context,vmcb,0x1000,0x2000,reinterpret_cast<AppleHVGPRFrame *>(guest),8));
    context->~Context();free(storage);free(guest);
}
int main() {cpuTests();abiTests();recoveryTests();nativeMapTests();transitionPreparation();puts("CPU rollback, inspected ABI, map contracts, fault probes and private-IDT preparation tests passed.");}
