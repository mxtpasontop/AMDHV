#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>
#include <src/InstructionDecoder.hpp>
#include <src/SoftVMCS.hpp>
#include <src/NPTManager.hpp>
#include "PhysicalFixture.hpp"
#include <src/VMCBTranslator.hpp>

struct OperandFixture {
    void *slots[3];
    static bool transfer(void *ctx,uint64_t address,void *out,const void *input,size_t n) {
        auto self=static_cast<OperandFixture *>(ctx);
        for (auto slot:self->slots) if (address==reinterpret_cast<uint64_t>(slot) && n==8) {
            if(input) memcpy(slot,input,n);else memcpy(out,slot,n);return true;
        }
        return false;
    }
    static bool read(void *c,uint64_t a,void *p,size_t n) {return transfer(c,a,p,nullptr,n);}
    static bool write(void *c,uint64_t a,const void *p,size_t n) {return transfer(c,a,nullptr,p,n);}
    static bool physical(void *,uint64_t a,void *p,size_t n) {
        if((a==0x100000 || a==0x200000) && n==4) {uint32_t revision=1;memcpy(p,&revision,4);return true;}return false;
    }
};
bool amdhvDebugEnabled = true;

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define EXPECT_EQ(actual, expected, msg) do { \
    uint64_t _a = static_cast<uint64_t>(actual); \
    uint64_t _e = static_cast<uint64_t>(expected); \
    if (_a == _e) { \
        gTestsPassed++; \
    } else { \
        gTestsFailed++; \
        printf("  [FAIL] %s: actual=0x%llx, expected=0x%llx (line %d)\n", msg, \
               (unsigned long long)_a, (unsigned long long)_e, __LINE__); \
    } \
} while (0)

#define EXPECT_TRUE(cond, msg) do { \
    if (cond) { \
        gTestsPassed++; \
    } else { \
        gTestsFailed++; \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

// ============================================================================
// Test 1: Kernel Trap (#UD / #GP) VMX & MSR Instruction Emulator
// ============================================================================
static void testTrapEmulator() {
    printf("[Test 1] Testing #UD / #GP Kernel Trap VMX & MSR Instruction Emulator...\n");
    SoftVMCS::init();
    NPTManager::init(0);

    XNUSavedState64 state{};
    state.cs     = 0x08; // Kernel mode CPL 0
    state.cpu    = 0;
    state.trapno = XNU_TRAP_INVALID_OPCODE;

    uint64_t vmxonRegionPA = 0x100000ULL;
    uint64_t vmcsRegionPA  = 0x200000ULL;
    uint64_t storedVmcsPA  = 0;
    OperandFixture operands{{&vmxonRegionPA,&vmcsRegionPA,&storedVmcsPA}};
    MemoryAccess::Access memory{&operands,OperandFixture::read,OperandFixture::write,OperandFixture::physical};

    // 1. Emulate: vmxon (%rdi)  -> F3 0F C7 37 (4 bytes)
    uint8_t insnVmxon[] = { 0xF3, 0x0F, 0xC7, 0x37 };
    state.rip    = reinterpret_cast<uint64_t>(insnVmxon);
    state.rdi    = reinterpret_cast<uint64_t>(&vmxonRegionPA);
    state.rflags = RFLAGS_CF | RFLAGS_ZF;
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmxon,sizeof(insnVmxon),memory,0), "Emulate vmxon (%rdi)");
    EXPECT_EQ(state.rip, reinterpret_cast<uint64_t>(insnVmxon) + 4, "vmxon advances RIP by 4");
    EXPECT_EQ(state.rflags & RFLAGS_VMX_MASK, 0, "vmxon clears RFLAGS CF/ZF");

    // 2. Emulate: vmclear (%rdi) -> 66 0F C7 37 (4 bytes)
    uint8_t insnVmclear[] = { 0x66, 0x0F, 0xC7, 0x37 };
    state.rip = reinterpret_cast<uint64_t>(insnVmclear);
    state.rdi = reinterpret_cast<uint64_t>(&vmcsRegionPA);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmclear,sizeof(insnVmclear),memory,0), "Emulate vmclear (%rdi)");
    EXPECT_EQ(state.rip, reinterpret_cast<uint64_t>(insnVmclear) + 4, "vmclear advances RIP by 4");

    // 3. Emulate: vmptrld (%rdi) -> 0F C7 37 (3 bytes)
    uint8_t insnVmptrld[] = { 0x0F, 0xC7, 0x37 };
    state.rip = reinterpret_cast<uint64_t>(insnVmptrld);
    state.rdi = reinterpret_cast<uint64_t>(&vmcsRegionPA);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmptrld,sizeof(insnVmptrld),memory,0), "Emulate vmptrld (%rdi)");
    EXPECT_EQ(state.rip, reinterpret_cast<uint64_t>(insnVmptrld) + 3, "vmptrld advances RIP by 3");

    // 4. Emulate: vmptrst (%rsi) -> 0F C7 3E (3 bytes)
    uint8_t insnVmptrst[] = { 0x0F, 0xC7, 0x3E };
    state.rip = reinterpret_cast<uint64_t>(insnVmptrst);
    state.rsi = reinterpret_cast<uint64_t>(&storedVmcsPA);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmptrst,sizeof(insnVmptrst),memory,0), "Emulate vmptrst (%rsi)");
    EXPECT_EQ(storedVmcsPA, vmcsRegionPA, "vmptrst stores active VMCS PA");

    // 5. Emulate: vmwrite %rsi, %rdi -> 0F 79 FE (ModRM: mod=3, reg=7(RDI), rm=6(RSI))
    uint8_t insnVmwrite[] = { 0x0F, 0x79, 0xFE };
    state.rip = reinterpret_cast<uint64_t>(insnVmwrite);
    state.rdi = VMCS_GUEST_RIP;
    state.rsi = 0xFFFFFF8000200000ULL;
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmwrite,sizeof(insnVmwrite),memory,0), "Emulate vmwrite %rsi, %rdi");
    EXPECT_EQ(state.rip, reinterpret_cast<uint64_t>(insnVmwrite) + 3, "vmwrite advances RIP by 3");

    // 6. Emulate: vmread %rdi, %rax -> 0F 78 F8 (ModRM: mod=3, reg=7(RDI), rm=0(RAX))
    uint8_t insnVmread[] = { 0x0F, 0x78, 0xF8 };
    state.rip = reinterpret_cast<uint64_t>(insnVmread);
    state.rdi = VMCS_GUEST_RIP;
    state.rax = 0;
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmread,sizeof(insnVmread),memory,0), "Emulate vmread %rdi, %rax");
    EXPECT_EQ(state.rax, 0xFFFFFF8000200000ULL, "vmread returns written VMCS_GUEST_RIP");
    state.rdi=uint64_t(VMCS_GUEST_RIP)|(1ULL<<32);state.rip=reinterpret_cast<uint64_t>(insnVmread);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmread,sizeof(insnVmread),memory,0),"VMREAD rejects high reserved encoding bits");
    EXPECT_EQ(state.rflags&RFLAGS_VMX_MASK,RFLAGS_ZF,"Unsupported full-width encoding reports VMfailValid");
    EXPECT_EQ(state.rax,0xFFFFFF8000200000ULL,"Failed VMREAD preserves destination");
    state.rsi=0xbad;state.rip=reinterpret_cast<uint64_t>(insnVmwrite);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmwrite,sizeof(insnVmwrite),memory,0),"VMWRITE rejects high reserved encoding bits");
    EXPECT_EQ(state.rflags&RFLAGS_VMX_MASK,RFLAGS_ZF,"Failed VMWRITE reports VMfailValid");
    state.rdi=VMCS_GUEST_RIP;state.rip=reinterpret_cast<uint64_t>(insnVmread);
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnVmread,sizeof(insnVmread),memory,0),"VMREAD succeeds after unsupported encoding");
    EXPECT_EQ(state.rax,0xFFFFFF8000200000ULL,"Failed VMWRITE preserves the original field");

    // 7. Emulate: invept (%rax), %rcx -> 66 0F 38 80 08 (5 bytes, matching XNU _pmap_flush_tlbs!)
    uint64_t inveptDesc[2] = { 0x30001eULL, 0ULL };
    uint8_t insnInvept[] = { 0x66, 0x0F, 0x38, 0x80, 0x08 };
    state.rip = reinterpret_cast<uint64_t>(insnInvept);
    state.rax = reinterpret_cast<uint64_t>(inveptDesc);
    memory.context=inveptDesc;
    memory.read=[](void *c,uint64_t a,void *p,size_t n) {if(a!=reinterpret_cast<uint64_t>(c)||n!=16)return false;memcpy(p,c,n);return true;};
    state.rcx = 1; // Single-context INVEPT
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnInvept,sizeof(insnInvept),memory,0), "Emulate XNU invept (%rax), %rcx");
    EXPECT_EQ(state.rip, reinterpret_cast<uint64_t>(insnInvept) + 5, "invept advances RIP by 5");

    // 8. Emulate: rdmsr (0F 32) on #GP (trapno = 13) for MSR_IA32_VMX_BASIC (0x480)
    uint8_t insnRdmsr[] = { 0x0F, 0x32 };
    state.trapno = XNU_TRAP_GENERAL_PROTECTION;
    state.rip    = reinterpret_cast<uint64_t>(insnRdmsr);
    state.rcx    = MSR_IA32_VMX_BASIC;
    state.rax    = 0;
    state.rdx    = 0;
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,insnRdmsr,sizeof(insnRdmsr),memory,0), "Emulate rdmsr on MSR_IA32_VMX_BASIC");
    uint64_t vmxBasic = (state.rax & 0xFFFFFFFFULL) | (state.rdx << 32);
    EXPECT_EQ(vmxBasic & 0x7FFFFFFFULL, 1ULL, "VMX_BASIC revision ID is 1");
    EXPECT_TRUE((vmxBasic & (1ULL << 55)) != 0, "VMX_BASIC bit 55 (True CTLS) is set");
    const uint8_t off[]={0x0f,0x01,0xc4};
    EXPECT_TRUE(SoftVMCS::emulateInstruction(&state,off,sizeof(off),memory,0),"VMXOFF releases CPU binding");
}

// ============================================================================
// Test 2: Verify Synthesized VMX MSRs against AppleHV.kext static_init Checks
// ============================================================================
static void testAppleHVMSRCompatibility() {
    printf("[Test 2] Verifying Synthesized VMX Capability MSRs against AppleHV.kext checks...\n");

    uint64_t eptCap = 0;
    EXPECT_TRUE(SoftVMCS::readSynthesizedMSR(MSR_IA32_VMX_EPT_VPID_CAP, eptCap), "Read MSR_IA32_VMX_EPT_VPID_CAP");

    // In AppleHV.kext hv_vmx_vcpu_t::static_init (0x39fd):
    // Required EPT/VPID capability mask is 0xF0106114040:
    uint64_t appleHVRequiredEptMask = 0x00000F0106114040ULL;
    EXPECT_TRUE((eptCap & appleHVRequiredEptMask) != appleHVRequiredEptMask,
              "Unsupported AppleHV requirements must not be fabricated");

    uint64_t procCtls = 0, procCtls2 = 0;
    EXPECT_TRUE(SoftVMCS::readSynthesizedMSR(MSR_IA32_VMX_TRUE_PROCBASED_CTLS, procCtls), "Read TRUE_PROCBASED_CTLS");
    EXPECT_TRUE(SoftVMCS::readSynthesizedMSR(MSR_IA32_VMX_PROCBASED_CTLS2, procCtls2), "Read PROCBASED_CTLS2");

    uint32_t proc1Allowed1 = static_cast<uint32_t>(procCtls >> 32);
    uint32_t proc2Allowed1 = static_cast<uint32_t>(procCtls2 >> 32);

    EXPECT_TRUE((proc1Allowed1 & CPU_BASED_ACTIVATE_SEC_CTLS) != 0, "Secondary controls allowed");
    EXPECT_TRUE((proc1Allowed1 & CPU_BASED_TPR_SHADOW) == 0, "Unsupported TPR shadow omitted");
    EXPECT_TRUE((proc2Allowed1 & CPU_BASED2_ENABLE_EPT) != 0, "EPT allowed");
    EXPECT_TRUE((proc2Allowed1 & CPU_BASED2_UNRESTRICTED_GUEST) != 0, "Unrestricted Guest allowed");
}

// ============================================================================
// Test 3: VMCS <-> VMCB State & Segment Access Rights Translation
// ============================================================================
static void testVMCStoVMCBTranslation() {
    printf("[Test 3] Testing VMCS <-> VMCB State & Segment Translation...\n");

    // 64-bit Code Segment: Intel AR = 0xA09B (Type=B, S=1, DPL=0, P=1, L=1, G=1) -> AMD Attrib = 0x0A9B
    uint16_t amdCS = VMCBTranslator::intelARToAMDAttrib(0x0000A09BU);
    EXPECT_EQ(amdCS, 0x0A9BU, "64-bit CS Intel AR 0xA09B -> AMD attrib 0x0A9B");
    EXPECT_EQ(VMCBTranslator::amdAttribToIntelAR(amdCS), 0x0000A09BU, "Round-trip 64-bit CS attrib -> Intel AR");

    // Unusable segment: Intel AR = 0x10000 -> AMD Attrib = 0x0000 -> Intel AR = 0x10000
    uint16_t amdUnusable = VMCBTranslator::intelARToAMDAttrib(0x00010000U);
    EXPECT_EQ(amdUnusable, 0x0000U, "Unusable Intel AR 0x10000 -> AMD attrib 0");
    EXPECT_EQ(VMCBTranslator::amdAttribToIntelAR(amdUnusable), 0x00010000U, "Unusable AMD attrib 0 -> Intel AR 0x10000");

    // Full SoftVMCS -> VMCB sync test
    auto vmcs = SoftVMCS::getOrCreateByPhysAddr(0x500000ULL);
    EXPECT_TRUE(bool(vmcs) && vmcs->vmcbVA != nullptr, "Allocate SoftVMCS + VMCB");

    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_RIP, 0xFFFFFF8000102030ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_RSP, 0xFFFF80000000F000ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_RFLAGS, 0x202ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_CR0, 0x80050033ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_CR3, 0x00100000ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_CR4, 0x000026F0ULL); // Note bit 13 (VMXE = 0x2000) set
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_CS, 0x08);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_CS_AR, 0xA09B);
    SoftVMCS::writeField(vmcs.get(), VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA | EFER_NXE);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_PIN_BASED, PIN_BASED_INTR_EXIT | PIN_BASED_NMI_EXIT);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_CPU_BASED, CPU_BASED_HLT_EXIT | CPU_BASED_UNCOND_IO_EXIT | CPU_BASED_ACTIVATE_SEC_CTLS | CPU_BASED_UNCOND_IO_EXIT);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_CPU_BASED2, CPU_BASED2_ENABLE_EPT | CPU_BASED2_UNRESTRICTED_GUEST);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_VMENTRY_IRQ_INFO, 0x80000020U); // Inject external interrupt vector 0x20

    AppleHVGPRFrame gprs{};
    gprs.rax = 0xDEADBEEFCAFEBABEULL;
    gprs.cr2 = 0x12345678ULL;

    EXPECT_TRUE(VMCBTranslator::syncVMCStoVMCB(vmcs.get(), &gprs, nullptr)==VMCBTranslator::EntryResult::MissingNPT,"Missing EPTP blocks entry after state translation");

    VMCB *vmcb = vmcs->vmcbVA;
    EXPECT_EQ(vmcb->state.rip, 0xFFFFFF8000102030ULL, "VMCB RIP synced");
    EXPECT_EQ(vmcb->state.rsp, 0xFFFF80000000F000ULL, "VMCB RSP synced");
    EXPECT_EQ(vmcb->state.rax, 0xDEADBEEFCAFEBABEULL, "VMCB RAX synced from GPRs");
    EXPECT_EQ(vmcb->state.cr4, 0x000006F0ULL, "VMCB CR4 strips Intel VMXE bit 13");
    EXPECT_EQ(vmcb->state.efer, EFER_LME | EFER_LMA | EFER_NXE | EFER_SVME, "VMCB EFER forces EFER_SVME=1");
    EXPECT_EQ(vmcb->control.event_inj, 0x80000020ULL, "VMCB EVENTINJ populated from VMENTRY_IRQ_INFO");
    EXPECT_TRUE((vmcb->control.intercept_ops1 & SVM_INTERCEPT_VINTR) == 0, "No interrupt window requested");
    EXPECT_TRUE((vmcb->control.vintr & SVM_VINTR_IRQ) == 0, "No virtual IRQ fabricated");

    // Simulate a CPUID #VMEXIT on AMD SVM (exit_code = 0x72, next_rip = rip + 2)
    vmcb->control.exit_code = SVM_VMEXIT_CPUID;
    vmcb->control.next_rip  = vmcb->state.rip + 2;
    vmcb->state.rax         = 0x00000001ULL;

    bool fastRetry = false;
    EXPECT_TRUE(VMCBTranslator::syncVMCBtoVMCS(vmcs.get(), &gprs, nullptr, fastRetry)==VMCBTranslator::ExitResult::GuestExit,"CPUID guest exit translated");
    EXPECT_TRUE(!fastRetry, "CPUID exit does not trigger fast-path retry");

    uint64_t exitReason = 0, instrLen = 0, guestEfer = 0, guestCr4 = 0;
    SoftVMCS::readField(vmcs.get(), VMCS_RO_EXIT_REASON, exitReason);
    SoftVMCS::readField(vmcs.get(), VMCS_RO_VMEXIT_INSTR_LEN, instrLen);
    SoftVMCS::readField(vmcs.get(), VMCS_GUEST_IA32_EFER, guestEfer);
    SoftVMCS::readField(vmcs.get(), VMCS_GUEST_CR4, guestCr4);

    EXPECT_EQ(exitReason, VMX_REASON_CPUID, "SVM_VMEXIT_CPUID -> VMX_REASON_CPUID (10)");
    EXPECT_EQ(instrLen, 2ULL, "NextRIP - RIP = 2 stored in VMCS_RO_VMEXIT_INSTR_LEN");
    EXPECT_EQ(guestEfer, EFER_LME | EFER_LMA | EFER_NXE, "Hardware EFER_SVME stripped from guest logical EFER");
    EXPECT_EQ(guestCr4, 0x000026F0ULL, "Guest logical CR4.VMXE preserved across VMEXIT");
}

// ============================================================================
// Test 4: Intel EPT -> Shadow AMD NPT 4-Level Walk & #NPF Fast-Path
// ============================================================================
static void testEPTtoNPTTranslation() {
    printf("[Test 4] Testing Intel EPT -> Shadow AMD NPT Page Table Sync & #NPF Handling...\n");

    // Allocate 4 mock EPT page table pages (PML4, PDPT, PD, PT) in userspace
    uint64_t *eptPml4 = nullptr, *eptPdpt = nullptr, *eptPd = nullptr, *eptPt = nullptr;
    if(posix_memalign(reinterpret_cast<void **>(&eptPml4),4096,4096)) abort();
    if(posix_memalign(reinterpret_cast<void **>(&eptPdpt),4096,4096)) abort();
    if(posix_memalign(reinterpret_cast<void **>(&eptPd),4096,4096)) abort();
    if(posix_memalign(reinterpret_cast<void **>(&eptPt),4096,4096)) abort();
    memset(eptPml4, 0, 4096);
    memset(eptPdpt, 0, 4096);
    memset(eptPd,   0, 4096);
    memset(eptPt,   0, 4096);

    PhysicalFixture physical;
    physical.add(eptPml4); physical.add(eptPdpt); physical.add(eptPd); physical.add(eptPt);
    EXPECT_TRUE(physical.install(), "Install bounded physical reader");
    uint64_t eptp = reinterpret_cast<uint64_t>(eptPml4) | 0x1EULL; // WB, 4-level walk
    uint64_t targetHPA = 0x0000000123456000ULL;
    physical.allowHPA(targetHPA);

    // Map GPA 0x1000 -> targetHPA with Read + Write (no Execute) and WB memtype (6 << 3)
    eptPml4[0] = reinterpret_cast<uint64_t>(eptPdpt) | NPTManager::EPT_READ | NPTManager::EPT_WRITE | NPTManager::EPT_EXEC;
    eptPdpt[0] = reinterpret_cast<uint64_t>(eptPd)   | NPTManager::EPT_READ | NPTManager::EPT_WRITE | NPTManager::EPT_EXEC;
    eptPd[0]   = reinterpret_cast<uint64_t>(eptPt)   | NPTManager::EPT_READ | NPTManager::EPT_WRITE | NPTManager::EPT_EXEC;
    eptPt[1]   = targetHPA | (6ULL << 3) | NPTManager::EPT_READ | NPTManager::EPT_WRITE; // GPA 0x1000: R+W, NX

    auto vmcs = SoftVMCS::getOrCreateByPhysAddr(0x600000ULL);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_CPU_BASED, CPU_BASED_ACTIVATE_SEC_CTLS | CPU_BASED_UNCOND_IO_EXIT);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_CPU_BASED2, CPU_BASED2_ENABLE_EPT);
    SoftVMCS::writeField(vmcs.get(), VMCS_CTRL_EPTP, eptp);

    AppleHVGPRFrame gprs{};
    EXPECT_TRUE(VMCBTranslator::syncVMCStoVMCB(vmcs.get(), &gprs, nullptr)==VMCBTranslator::EntryResult::Ready,"Mapped NPT root prepares model entry");

    // Simulate an AMD #NPF exit on GPA 0x1000 (not yet synced into shadow NPT) for a data write
    VMCB *vmcb = vmcs->vmcbVA;
    vmcb->control.exit_code   = SVM_VMEXIT_NPF;
    vmcb->control.exit_info_1 = SVM_NPF_INFO1_WRITE | SVM_NPF_INFO1_USER | SVM_NPF_INFO1_FINAL_GPA; // Present=0
    vmcb->control.exit_info_2 = 0x1000ULL;

    bool fastRetry = false;
    EXPECT_TRUE(VMCBTranslator::syncVMCBtoVMCS(vmcs.get(), &gprs, nullptr, fastRetry)==VMCBTranslator::ExitResult::Retry,"NPF model retry");
    EXPECT_TRUE(fastRetry, "#NPF on valid EPT PTE syncs shadow NPT and triggers fast-path retry without exiting to AppleHV");

    // Now simulate an instruction fetch on GPA 0x1000 where Present=1 in NPT, but NX=1 (no EPT_EXEC)
    vmcb->control.exit_code   = SVM_VMEXIT_NPF;
    vmcb->control.exit_info_1 = SVM_NPF_INFO1_PRESENT | SVM_NPF_INFO1_FETCH | SVM_NPF_INFO1_USER | SVM_NPF_INFO1_FINAL_GPA;
    vmcb->control.exit_info_2 = 0x1000ULL;

    EXPECT_TRUE(VMCBTranslator::syncVMCBtoVMCS(vmcs.get(), &gprs, nullptr, fastRetry)==VMCBTranslator::ExitResult::GuestExit,"NPF permission exit");
    EXPECT_TRUE(!fastRetry, "Permission violation (#NPF with Present=1, Fetch=1 on NX page) forwards to AppleHV");

    uint64_t exitReason = 0, exitQual = 0, faultGPA = 0;
    SoftVMCS::readField(vmcs.get(), VMCS_RO_EXIT_REASON, exitReason);
    SoftVMCS::readField(vmcs.get(), VMCS_RO_EXIT_QUALIFIC, exitQual);
    SoftVMCS::readField(vmcs.get(), VMCS_GUEST_PHYSICAL_ADDRESS, faultGPA);

    EXPECT_EQ(exitReason, VMX_REASON_EPT_VIOLATION, "SVM_VMEXIT_NPF -> VMX_REASON_EPT_VIOLATION (48)");
    EXPECT_EQ(faultGPA, 0x1000ULL, "Faulting GPA 0x1000 recorded in VMCS_GUEST_PHYSICAL_ADDRESS");
    // Qualification should have: Bit 2 (Fetch=1), Bit 3 (EPT_R=1), Bit 4 (EPT_W=1), Bit 5 (EPT_X=0), Bit 7 (GLA valid), Bit 8 (Final GPA)
    EXPECT_EQ(exitQual, (1ULL << 2) | (1ULL << 3) | (1ULL << 4) ,
              "EPT Violation Exit Qualification matches Intel SDM Table 27-7");

    NPTManager::releaseEPTP(eptp);
    EXPECT_TRUE(NPTManager::shutdown(),"Release source adapter before fixture teardown");
    free(eptPml4);
    free(eptPdpt);
    free(eptPd);
    free(eptPt);
}

int main() {
    printf("================================================================\n");
    printf(" AMDHV.kext Translation Engine Unit Test Suite\n");
    printf("================================================================\n");

    testTrapEmulator();
    testAppleHVMSRCompatibility();
    testVMCStoVMCBTranslation();
    testEPTtoNPTTranslation();

    printf("================================================================\n");
    EXPECT_TRUE(SoftVMCS::shutdown(),"All VMCS leases released at teardown");
    printf(" Results: %d passed, %d failed\n", gTestsPassed, gTestsFailed);
    printf("================================================================\n");
    return (gTestsFailed == 0) ? 0 : 1;
}
