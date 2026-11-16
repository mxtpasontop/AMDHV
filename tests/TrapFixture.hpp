#pragma once
#include <src/TrapAdapter.hpp>
struct TrapFixture {
    VcpuFixture &vcpu;
    TrapAdapter::Adapter adapter;
    uint8_t code[15] {};
    size_t length {};
    alignas(8) uint64_t stack[64] {};
    const uint64_t constant=0x123456789;
    bool interrupts=true,enabled=true,installed=true,allowCode=true,nested=false;
    unsigned pins {},unpins {};
    uintptr_t thread=1;
    uint32_t cpu=0;
    explicit TrapFixture(VcpuFixture &owner):vcpu(owner) {
        TrapAdapter::Operations ops;ops.context=this;ops.vcpus=vcpu.registry.get();
        ops.faults={this,[](void *p) {return static_cast<TrapFixture *>(p)->cpu;},
            [](void *p) {return static_cast<TrapFixture *>(p)->thread;},
            [](void *p) {auto &m=*static_cast<TrapFixture *>(p);bool previous=m.interrupts;m.interrupts=false;return previous;},
            [](void *p,bool previous) {static_cast<TrapFixture *>(p)->interrupts=previous;},
            [](void *p) {return static_cast<TrapFixture *>(p)->installed;},nullptr};
        ops.pinCode=[](void *p,uint64_t pc,TrapAdapter::Code &out) {
            auto &m=*static_cast<TrapFixture *>(p);CHECK(!m.interrupts);
            if(!m.allowCode || pc!=reinterpret_cast<uint64_t>(m.code)) return false;
            CHECK(!m.pins);++m.pins;out={pc,m.length,1};
            if(m.nested) {m.nested=false;auto state=m.state();CHECK(!m.adapter.handle(&state) && !m.interrupts);}
            return true;
        };
        ops.unpinCode=[](void *p,const TrapAdapter::Code &code) {auto &m=*static_cast<TrapFixture *>(p);CHECK(code.token==1 && m.pins==1 && !m.interrupts);--m.pins;++m.unpins;};
        ops.readOnly=[](void *p,const TrapAdapter::Code &,uint64_t address,size_t bytes) {
            auto &m=*static_cast<TrapFixture *>(p);return address==reinterpret_cast<uint64_t>(&m.constant) && bytes<=sizeof(m.constant);
        };
        ops.stack=[](void *p,uintptr_t thread,VcpuLifecycle::Buffer &out) {
            auto &m=*static_cast<TrapFixture *>(p);CHECK(thread==m.thread);out={reinterpret_cast<uint64_t>(m.stack),sizeof(m.stack)};return true;
        };
        ops.readyCPU=[](void *p,uint32_t cpu) {auto &m=*static_cast<TrapFixture *>(p);CHECK(cpu==m.cpu && !m.interrupts);return m.enabled;};
        CHECK(adapter.configure(ops) && !adapter.configure(ops));
    }
    XNUSavedState64 state() {
        XNUSavedState64 s{};s.flavor=15;s.cs=8;s.trapno=XNU_TRAP_INVALID_OPCODE;
        s.rip=reinterpret_cast<uint64_t>(code);s.rsp=reinterpret_cast<uint64_t>(stack+32);s.rdi=reinterpret_cast<uint64_t>(stack);return s;
    }
    XNUSavedState64 instruction(std::initializer_list<uint8_t> bytes) {
        CHECK(bytes.size()<=sizeof(code));length=bytes.size();memset(code,0,sizeof(code));size_t i=0;for(auto byte:bytes) code[i++]=byte;return state();
    }
    bool handle(XNUSavedState64 &state) {
        bool previous=interrupts;bool result=adapter.handle(&state);CHECK(interrupts==previous && !pins);return result;
    }
};
static void trapOwnership() {
    CHECK(SoftVMCS::prepareResources(2,8));
    {
        VcpuFixture v;TrapFixture m(v);
        auto s=m.instruction({0x0f,0x32});s.rcx=MSR_IA32_VMX_BASIC;m.enabled=false;
        CHECK(m.handle(s) && (s.rax&0x7fffffff)==1 && m.unpins==1);
        s=m.state();s.rcx=MSR_IA32_VMX_BASIC;m.allowCode=false;CHECK(!m.handle(s));m.allowCode=true;
        s.cs=3;CHECK(!m.handle(s));s=m.state();s.flavor=0;CHECK(!m.handle(s));
        s=m.state();s.trapno=14;CHECK(!m.handle(s));s=m.state();s.rcx=MSR_IA32_VMX_BASIC;m.installed=false;CHECK(!m.handle(s));m.installed=true;
        m.cpu=128;CHECK(!m.handle(s));m.cpu=0;m.thread=0;CHECK(!m.handle(s));m.thread=1;
        s=m.state();s.rcx=MSR_IA32_VMX_BASIC;m.nested=true;CHECK(m.handle(s) && !m.nested);
        m.interrupts=false;s=m.state();s.rcx=MSR_IA32_VMX_BASIC;CHECK(m.handle(s));m.interrupts=true;
        m.length=1;s=m.state();CHECK(!m.handle(s));
        auto init=v.registry->begin(v.address(),v.key);CHECK(init);v.allocateCS();m.stack[0]=0x5000;
        s=m.instruction({0x66,0x0f,0xc7,0x37});uint64_t pc=s.rip;
        CHECK(!m.handle(s) && s.rip==pc && !SoftVMCS::findByPhysAddr(0x5000));
        m.enabled=true;s.rdi=reinterpret_cast<uint64_t>(&m.constant);CHECK(m.handle(s) && (s.rflags&RFLAGS_CF)); // bad PA, read-only owned constant
        s=m.state();CHECK(m.handle(s) && !(s.rflags&RFLAGS_VMX_MASK));
        s=m.instruction({0x0f,0xc7,0x37});CHECK(m.handle(s) && !(s.rflags&RFLAGS_VMX_MASK)); // VMPTRLD
        s=m.instruction({0x0f,0x79,0xc8});s.rcx=VMCS_GUEST_RIP;s.rax=0x1234;CHECK(m.handle(s));
        s=m.instruction({0x0f,0x78,0xc8});s.rcx=VMCS_GUEST_RIP;CHECK(m.handle(s) && s.rax==0x1234);
        s=m.instruction({0x0f,0x78,0x0f});s.rcx=VMCS_GUEST_RIP;CHECK(m.handle(s) && m.stack[0]==0x1234);
        uint64_t unrelated=0xfeed;
        s=m.state();s.rcx=VMCS_GUEST_RIP;s.rdi=reinterpret_cast<uint64_t>(&unrelated);pc=s.rip;
        CHECK(!m.handle(s) && unrelated==0xfeed && s.rip==pc);
        s=m.state();s.rcx=VMCS_GUEST_RIP;s.rdi=reinterpret_cast<uint64_t>(v.ro)+24;CHECK(!m.handle(s));
        s=m.state();s.rcx=VMCS_GUEST_RIP;s.rdi=reinterpret_cast<uint64_t>(m.stack)+1;CHECK(!m.handle(s));
        s=m.state();s.rcx=VMCS_GUEST_RIP;s.rsp=0;CHECK(!m.handle(s));
        s=m.state();s.rcx=VMCS_GUEST_RIP;s.rdi=reinterpret_cast<uint64_t>(v.rw)+4096;CHECK(m.handle(s)); // retained RW page
        uint64_t written=0;memcpy(&written,v.rw+4096,8);CHECK(written==0x1234);
        s=m.instruction({0x0f,0x79,0x0f});s.rcx=VMCS_GUEST_RIP;s.rdi=reinterpret_cast<uint64_t>(&m.constant);CHECK(m.handle(s));
        s=m.instruction({0x0f,0x78,0xc8});s.rcx=VMCS_GUEST_RIP;CHECK(m.handle(s) && s.rax==m.constant);
        s=m.instruction({0x66,0x0f,0x38,0x80,0x0f});s.rcx=1;m.stack[0]=v.key.eptp();m.stack[1]=0;pc=s.rip;
        CHECK(!m.handle(s) && s.rip==pc && !v.tables.flushes);
        auto update=NPTManager::beginUpdate(v.key);CHECK(update && NPTManager::acknowledgeRevoked(v.key.eptp()));
        CHECK(m.handle(s) && !v.tables.flushes);update.reset();CHECK(!NPTManager::acknowledgeRevoked(v.key.eptp()));
        s=m.instruction({0xf3,0x0f,0xc7,0x37});CHECK(!m.handle(s));
        s=m.instruction({0x0f,0x01,0xc4});CHECK(!m.handle(s));
        m.stack[0]=0x5000;s=m.instruction({0x66,0x0f,0xc7,0x37});CHECK(m.handle(s));
        CHECK(init.finish(true));auto retirement=v.registry->retire(v.address());CHECK(retirement);retirement.reset();
        // An active unowned slot is refused even for a register-only access.
        auto core=SoftVMCS::getOrCreateByPhysAddr(0x6000);CHECK(core && SoftVMCS::setActiveVMCS(0,core));core.reset();
        s=m.instruction({0x0f,0x78,0xc8});s.rcx=VMCS_GUEST_RIP;CHECK(!m.handle(s));
        CHECK(SoftVMCS::clearActiveVMCS(0) && SoftVMCS::releaseByPhysAddr(0x6000));
    }
    CHECK(SoftVMCS::shutdown());
}
