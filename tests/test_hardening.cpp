#include <src/SoftVMCS.hpp>
#include <src/NPTManager.hpp>
#include "PhysicalFixture.hpp"
#include <src/VMCBTranslator.hpp>
#include <src/InstructionDecoder.hpp>
#include <src/BackendPolicy.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
bool amdhvDebugEnabled = false;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CHECK %s at %d\n",#x,__LINE__); abort(); } } while(0)
static uint64_t *page() {
    void *p = nullptr;
    CHECK(posix_memalign(&p,4096,4096)==0);
    memset(p,0,4096);
    return static_cast<uint64_t *>(p);
}
int main() {
    CHECK(!BackendPolicy::executionEnabled);
    BackendPolicy::Mode mode;
    CHECK(BackendPolicy::parseMode("preflight",mode) && mode==BackendPolicy::Mode::Preflight);
    CHECK(!BackendPolicy::parseMode("",mode));
    CHECK(!BackendPolicy::parseMode("unknown",mode));
    CHECK(SoftVMCS::fieldEncodingToIndex(0x781e)==-1);
    CHECK(SoftVMCS::fieldEncodingToIndex(0x681f)==-1);
    CHECK(SoftVMCS::fieldEncodingToIndex(0x681e)>=0);
    CHECK(!SoftVMCS::getOrCreateByPhysAddr(0));
    CHECK(!SoftVMCS::getOrCreateByPhysAddr(0x123));
    // More distinct lifetimes than the fixed slot count must be reusable.
    for (unsigned i=1;i<200;++i) {
        auto v=SoftVMCS::getOrCreateByPhysAddr(uint64_t(i)<<12);
        CHECK(v && v->vmcbVA);
        for(unsigned j=0;j<12288;++j) CHECK(v->iopmVA[j]==0xff);
        v.reset();
        SoftVMCS::releaseByPhysAddr(uint64_t(i)<<12);
        CHECK(!SoftVMCS::findByPhysAddr(uint64_t(i)<<12));
    }
    XNUSavedState64 state{};
    InstructionDecoder::DecodedInstruction instruction;
    const uint8_t vmxon[]={0xf3,0x0f,0xc7,0x37};
    for(size_t n=0;n<sizeof(vmxon);++n)
        CHECK(!InstructionDecoder::decodeInstruction(vmxon,n,&state,instruction));
    CHECK(InstructionDecoder::decodeInstruction(vmxon,sizeof(vmxon),&state,instruction));
    const uint8_t forbidden[]={0x67,0xf3,0x0f,0xc7,0x37};
    CHECK(!InstructionDecoder::decodeInstruction(forbidden,sizeof(forbidden),&state,instruction));
    // Exhaustive short buffers also exercise every truncation boundary under ASan.
    for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
        uint8_t bytes[]={uint8_t(a),uint8_t(b)};
        auto length=InstructionDecoder::decodeInstructionLength64(bytes,2);
        CHECK(length<=2);
        InstructionDecoder::decodeInstruction(bytes,2,&state,instruction);
    }
    // Exercise ModRM/SIB/displacement and prefix combinations beyond two bytes.
    uint32_t random=0x12345678;
    for(unsigned i=0;i<100000;++i) {
        uint8_t bytes[15];
        for(auto &byte:bytes) {random=random*1664525U+1013904223U;byte=uint8_t(random>>24);}
        size_t n=i%16;
        CHECK(InstructionDecoder::decodeInstructionLength64(bytes,n)<=n);
        if(InstructionDecoder::decodeInstruction(bytes,n,&state,instruction)) CHECK(instruction.length && instruction.length<=n);
    }
    CHECK(!MemoryAccess::span(UINT64_MAX,2));
    CHECK(!MemoryAccess::span(0x0000800000000000ULL,1));
    CHECK(MemoryAccess::span(0xffff800000000000ULL,8));
    state.cs=8;state.trapno=XNU_TRAP_INVALID_OPCODE;state.rip=0x1000;
    XNUSavedState64 before=state;
    MemoryAccess::Access unavailable;
    CHECK(!SoftVMCS::emulateInstruction(&state,vmxon,sizeof(vmxon),unavailable,0));
    CHECK(!memcmp(&state,&before,sizeof(state)));
    CHECK(!NPTManager::convertEPTEntryToNPT(0x1000|4|(6<<3),true));
    CHECK(!NPTManager::convertEPTEntryToNPT(0x1000|7,true));
    auto pml4=page(),pdpt=page(),pd=page(),pt=page();
    PhysicalFixture physical;
    physical.add(pml4);physical.add(pdpt);physical.add(pd);physical.add(pt);
    physical.allowHPA(0x12345000);
    CHECK(physical.install());
    uint64_t eptp=reinterpret_cast<uint64_t>(pml4)|0x1e;
    pml4[0]=reinterpret_cast<uint64_t>(pdpt)|7;
    pdpt[0]=reinterpret_cast<uint64_t>(pd)|7;
    pd[0]=reinterpret_cast<uint64_t>(pt)|7;
    pt[1]=0x12345000|7|(6<<3);
    auto v=SoftVMCS::getOrCreateByPhysAddr(0x200000);
    CHECK(v);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_CPU_BASED,CPU_BASED_ACTIVATE_SEC_CTLS|CPU_BASED_UNCOND_IO_EXIT);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_CPU_BASED2,CPU_BASED2_ENABLE_EPT);
    SoftVMCS::writeField(v.get(),VMCS_CTRL_EPTP,eptp);
    AppleHVGPRFrame gprs{};
    CHECK(VMCBTranslator::syncVMCStoVMCB(v.get(),&gprs)==VMCBTranslator::EntryResult::Ready);
    v->vmcbVA->state.rip=0x777;
    v->vmcbVA->state.rax=0xabc;
    v->vmcbVA->state.g_pat=0;
    v->vmcbVA->control.exit_code=SVM_VMEXIT_NPF;
    v->vmcbVA->control.exit_info_1=SVM_NPF_INFO1_WRITE|SVM_NPF_INFO1_FINAL_GPA;
    v->vmcbVA->control.exit_info_2=0x1000;
    bool retry=false;
    CHECK(VMCBTranslator::syncVMCBtoVMCS(v.get(),&gprs,nullptr,retry)==VMCBTranslator::ExitResult::Retry);
    CHECK(retry && gprs.rax==0xabc);
    CHECK(VMCBTranslator::syncVMCStoVMCB(v.get(),&gprs)==VMCBTranslator::EntryResult::Ready);
    CHECK(v->vmcbVA->state.rip==0x777 && v->vmcbVA->state.g_pat==0);
    uint64_t generation;
    auto root=reinterpret_cast<uint64_t *>(NPTManager::getOrCreateNCR3(eptp,generation));
    CHECK(root && root[0]);
    NPTManager::invalidateEPTP(eptp);
    CHECK(root[0]==0);
    uint64_t newGeneration;
    NPTManager::getOrCreateNCR3(eptp,newGeneration);
    CHECK(newGeneration>generation);
    CHECK(!NPTManager::syncGPAFromEPT(eptp,1ULL<<48));
    NPTManager::releaseEPTP(eptp);
    v.reset();
    SoftVMCS::releaseByPhysAddr(0x200000);
    CHECK(SoftVMCS::shutdown());CHECK(NPTManager::shutdown());
    free(pml4);free(pdpt);free(pd);free(pt);
    puts("Hardening regression and bounded decoder tests passed.");
}
