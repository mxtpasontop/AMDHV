#include <src/SoftVMCS.hpp>
#include <src/TrapAdapter.hpp>
#include <src/InstructionDecoder.hpp>
#include <src/NPTManager.hpp>
#include <src/Memory.hpp>
#include <src/Architecture.hpp>
#include <src/BackendPolicy.hpp>
#include <src/Runtime.hpp>
#include <Headers/Lilu/kern_util.hpp>

#if defined(AMDHV_KERNEL_BUILD)
#include <IOKit/IOLib.h>
extern "C" {
    uint32_t cpu_number(void);
    uint64_t kvtophys(vm_offset_t va);
}
#endif

namespace SoftVMCS {

static SoftVMCSInstance gVMCSPool[MAX_VMCS_SLOTS];
static SoftVMCSInstance *gActiveVMCSPerCPU[MAX_CPUS];
static uint64_t gHostSavePA[MAX_CPUS];
static void *gHostSaveVA[MAX_CPUS];

static Runtime::Gate gRegistry;
static Runtime::Gate gEmulator;
static uint64_t gVMXON[128] {};
static uint8_t *gSharedIOPMVA = nullptr;
static uint64_t gSharedIOPMPA = 0;
static uint8_t *gSharedMSRPMVA = nullptr;
static uint64_t gSharedMSRPMPA = 0;
static bool gPrepared = false;
static uint32_t gSlotLimit = 0;
static uint64_t gLifetime = 0;

static void freeResources() {
    for (auto &slot : gVMCSPool) {
        Memory::release(slot.vmcbVA, 4096);
        // Avoid a VMCB/field-sized aggregate temporary on the kernel stack.
        static_assert(__is_trivially_copyable(SoftVMCSInstance),"in-place reset requires a trivial record");
        memset(&slot,0,sizeof(slot));
        slot.ownerCPU=MAX_CPUS;slot.lastCPU=MAX_CPUS;slot.asid=1;
    }
    for (size_t i = 0; i < MAX_CPUS; ++i) {
        Memory::release(gHostSaveVA[i], 4096);
        gHostSaveVA[i] = nullptr; gHostSavePA[i] = 0;
        gActiveVMCSPerCPU[i] = nullptr;
    }
    Memory::release(gSharedIOPMVA, 12288);
    Memory::release(gSharedMSRPMVA, 8192);
    gSharedIOPMVA = gSharedMSRPMVA = nullptr;
    gSharedIOPMPA = gSharedMSRPMPA = 0;
    gPrepared = false; gSlotLimit = 0;
}

bool prepareResources(uint32_t cpuCount, uint32_t hardwareASIDCount) {
    if (!cpuCount || cpuCount > MAX_CPUS || hardwareASIDCount < 2) return false;
    Runtime::Guard guard(gRegistry);
    if (!guard || gPrepared) return false;
    gSlotLimit = hardwareASIDCount - 1 < MAX_VMCS_SLOTS ? hardwareASIDCount - 1 : MAX_VMCS_SLOTS;
    gSharedIOPMVA = static_cast<uint8_t *>(Memory::allocate(12288, gSharedIOPMPA));
    gSharedMSRPMVA = static_cast<uint8_t *>(Memory::allocate(8192, gSharedMSRPMPA));
    if (!gSharedIOPMVA || !gSharedMSRPMVA) { freeResources(); return false; }
    memset(gSharedIOPMVA, 0xff, 12288);
    memset(gSharedMSRPMVA, 0xff, 8192);
    for (size_t i = 0; i < gSlotLimit; ++i) {
        auto &slot = gVMCSPool[i];
        slot.vmcbVA = static_cast<VMCB *>(Memory::allocate(4096, slot.vmcbPA));
        if (!slot.vmcbVA) { freeResources(); return false; }
        slot.iopmVA = gSharedIOPMVA; slot.iopmPA = gSharedIOPMPA;
        slot.msrpmVA = gSharedMSRPMVA; slot.msrpmPA = gSharedMSRPMPA;
        slot.asid = static_cast<uint32_t>(i + 1); // ASID zero is host-only.
    }
    for (size_t i = 0; i < cpuCount; ++i) {
        gHostSaveVA[i] = Memory::allocate(4096, gHostSavePA[i]);
        if (!gHostSaveVA[i]) { freeResources(); return false; }
    }
    gPrepared = true;
    return true;
}

void init() {
#ifndef AMDHV_KERNEL_BUILD
    // Standalone offline model convenience; kernel setup is explicitly sleepable.
    prepareResources(1, MAX_VMCS_SLOTS + 1);
#endif
}

bool shutdown() {
    Runtime::Guard emulator(gEmulator);
    if (!emulator) return false;
    Runtime::Guard guard(gRegistry);
    if (!guard) return false;
    for (auto region : gVMXON) if (region) return false;
    for (auto &slot : gVMCSPool)
        if (__atomic_load_n(&slot.leased, __ATOMIC_ACQUIRE) || slot.ownerCPU != MAX_CPUS) return false;
    freeResources();
    return true;
}

uint64_t getHostSavePA(uint32_t cpuId) {
    Runtime::Guard guard(gRegistry);
    return guard && gPrepared && cpuId < MAX_CPUS ? gHostSavePA[cpuId] : 0;
}

int fieldEncodingToIndex(uint32_t encoding) {
    // Intel VMCS encoding format:
    // Bits [14:13]: width (0=16-bit, 1=64-bit, 2=32-bit, 3=natural-width)
    // Bits [11:10]: type  (0=control, 1=read-only, 2=guest, 3=host)
    // Bits [9:1]:   index (0..31)
    // Bit  [0]:     high-half of 64-bit field (0=full, 1=high32)
    if (encoding & 0xFFFF9000U) {
        return -1;
    }
    switch (encoding & ~1U) {
        case VMCS_VPID:
        case VMCS_CTRL_POSTED_INT_NOTIFY_VECTOR:
        case VMCS_CTRL_EPTP_INDEX:
        case VMCS_GUEST_ES:
        case VMCS_GUEST_CS:
        case VMCS_GUEST_SS:
        case VMCS_GUEST_DS:
        case VMCS_GUEST_FS:
        case VMCS_GUEST_GS:
        case VMCS_GUEST_LDTR:
        case VMCS_GUEST_TR:
        case VMCS_GUEST_INT_STATUS:
        case VMCS_GUEST_PML_INDEX:
        case VMCS_HOST_ES:
        case VMCS_HOST_CS:
        case VMCS_HOST_SS:
        case VMCS_HOST_DS:
        case VMCS_HOST_FS:
        case VMCS_HOST_GS:
        case VMCS_HOST_TR:
        case VMCS_CTRL_IO_BITMAP_A:
        case VMCS_CTRL_IO_BITMAP_B:
        case VMCS_CTRL_MSR_BITMAPS:
        case VMCS_CTRL_VMEXIT_MSR_STORE_ADDR:
        case VMCS_CTRL_VMEXIT_MSR_LOAD_ADDR:
        case VMCS_CTRL_VMENTRY_MSR_LOAD_ADDR:
        case VMCS_CTRL_EXECUTIVE_VMCS_PTR:
        case VMCS_CTRL_PML_ADDR:
        case VMCS_CTRL_TSC_OFFSET:
        case VMCS_CTRL_VIRTUAL_APIC:
        case VMCS_CTRL_APIC_ACCESS:
        case VMCS_CTRL_POSTED_INT_DESC_ADDR:
        case VMCS_CTRL_VMFUNC_CONTROLS:
        case VMCS_CTRL_EPTP:
        case VMCS_CTRL_EOI_EXIT_BITMAP_0:
        case VMCS_CTRL_EOI_EXIT_BITMAP_1:
        case VMCS_CTRL_EOI_EXIT_BITMAP_2:
        case VMCS_CTRL_EOI_EXIT_BITMAP_3:
        case VMCS_CTRL_EPTP_LIST_ADDR:
        case VMCS_CTRL_VMREAD_BITMAP_ADDR:
        case VMCS_CTRL_VMWRITE_BITMAP_ADDR:
        case VMCS_CTRL_VIRT_EXC_INFO_ADDR:
        case VMCS_CTRL_XSS_EXITING_BITMAP:
        case VMCS_CTRL_ENCLS_EXITING_BITMAP:
        case VMCS_CTRL_TSC_MULTIPLIER:
        case VMCS_GUEST_PHYSICAL_ADDRESS:
        case VMCS_GUEST_LINK_POINTER:
        case VMCS_GUEST_IA32_DEBUGCTL:
        case VMCS_GUEST_IA32_PAT:
        case VMCS_GUEST_IA32_EFER:
        case VMCS_GUEST_IA32_PERF_GLOBAL_CTRL:
        case VMCS_GUEST_PDPTE0:
        case VMCS_GUEST_PDPTE1:
        case VMCS_GUEST_PDPTE2:
        case VMCS_GUEST_PDPTE3:
        case VMCS_GUEST_IA32_BNDCFGS:
        case VMCS_HOST_IA32_PAT:
        case VMCS_HOST_IA32_EFER:
        case VMCS_HOST_IA32_PERF_GLOBAL_CTRL:
        case VMCS_CTRL_PIN_BASED:
        case VMCS_CTRL_CPU_BASED:
        case VMCS_CTRL_EXC_BITMAP:
        case VMCS_CTRL_PF_ERROR_MASK:
        case VMCS_CTRL_PF_ERROR_MATCH:
        case VMCS_CTRL_CR3_COUNT:
        case VMCS_CTRL_VMEXIT_CONTROLS:
        case VMCS_CTRL_VMEXIT_MSR_STORE_COUNT:
        case VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT:
        case VMCS_CTRL_VMENTRY_CONTROLS:
        case VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT:
        case VMCS_CTRL_VMENTRY_IRQ_INFO:
        case VMCS_CTRL_VMENTRY_EXC_ERROR:
        case VMCS_CTRL_VMENTRY_INSTR_LEN:
        case VMCS_CTRL_TPR_THRESHOLD:
        case VMCS_CTRL_CPU_BASED2:
        case VMCS_CTRL_PLE_GAP:
        case VMCS_CTRL_PLE_WINDOW:
        case VMCS_RO_INSTR_ERROR:
        case VMCS_RO_EXIT_REASON:
        case VMCS_RO_VMEXIT_IRQ_INFO:
        case VMCS_RO_VMEXIT_IRQ_ERROR:
        case VMCS_RO_IDT_VECTOR_INFO:
        case VMCS_RO_IDT_VECTOR_ERROR:
        case VMCS_RO_VMEXIT_INSTR_LEN:
        case VMCS_RO_VMEXIT_INSTR_INFO:
        case VMCS_GUEST_ES_LIMIT:
        case VMCS_GUEST_CS_LIMIT:
        case VMCS_GUEST_SS_LIMIT:
        case VMCS_GUEST_DS_LIMIT:
        case VMCS_GUEST_FS_LIMIT:
        case VMCS_GUEST_GS_LIMIT:
        case VMCS_GUEST_LDTR_LIMIT:
        case VMCS_GUEST_TR_LIMIT:
        case VMCS_GUEST_GDTR_LIMIT:
        case VMCS_GUEST_IDTR_LIMIT:
        case VMCS_GUEST_ES_AR:
        case VMCS_GUEST_CS_AR:
        case VMCS_GUEST_SS_AR:
        case VMCS_GUEST_DS_AR:
        case VMCS_GUEST_FS_AR:
        case VMCS_GUEST_GS_AR:
        case VMCS_GUEST_LDTR_AR:
        case VMCS_GUEST_TR_AR:
        case VMCS_GUEST_IGNORE_IRQ:
        case VMCS_GUEST_ACTIVITY_STATE:
        case VMCS_GUEST_SMBASE:
        case VMCS_GUEST_IA32_SYSENTER_CS:
        case VMCS_GUEST_VMX_TIMER_VALUE:
        case VMCS_HOST_IA32_SYSENTER_CS:
        case VMCS_CTRL_CR0_MASK:
        case VMCS_CTRL_CR4_MASK:
        case VMCS_CTRL_CR0_SHADOW:
        case VMCS_CTRL_CR4_SHADOW:
        case VMCS_CTRL_CR3_VALUE0:
        case VMCS_CTRL_CR3_VALUE1:
        case VMCS_CTRL_CR3_VALUE2:
        case VMCS_CTRL_CR3_VALUE3:
        case VMCS_RO_EXIT_QUALIFIC:
        case VMCS_RO_IO_RCX:
        case VMCS_RO_IO_RSI:
        case VMCS_RO_IO_RDI:
        case VMCS_RO_IO_RIP:
        case VMCS_RO_GUEST_LIN_ADDR:
        case VMCS_GUEST_CR0:
        case VMCS_GUEST_CR3:
        case VMCS_GUEST_CR4:
        case VMCS_GUEST_ES_BASE:
        case VMCS_GUEST_CS_BASE:
        case VMCS_GUEST_SS_BASE:
        case VMCS_GUEST_DS_BASE:
        case VMCS_GUEST_FS_BASE:
        case VMCS_GUEST_GS_BASE:
        case VMCS_GUEST_LDTR_BASE:
        case VMCS_GUEST_TR_BASE:
        case VMCS_GUEST_GDTR_BASE:
        case VMCS_GUEST_IDTR_BASE:
        case VMCS_GUEST_DR7:
        case VMCS_GUEST_RSP:
        case VMCS_GUEST_RIP:
        case VMCS_GUEST_RFLAGS:
        case VMCS_GUEST_DEBUG_EXC:
        case VMCS_GUEST_SYSENTER_ESP:
        case VMCS_GUEST_SYSENTER_EIP:
        case VMCS_HOST_CR0:
        case VMCS_HOST_CR3:
        case VMCS_HOST_CR4:
        case VMCS_HOST_FS_BASE:
        case VMCS_HOST_GS_BASE:
        case VMCS_HOST_TR_BASE:
        case VMCS_HOST_GDTR_BASE:
        case VMCS_HOST_IDTR_BASE:
        case VMCS_HOST_IA32_SYSENTER_ESP:
        case VMCS_HOST_IA32_SYSENTER_EIP:
        case VMCS_HOST_RSP:
        case VMCS_HOST_RIP:
            break;
        default: return -1;
    }
    uint32_t width = (encoding >> 13) & 0x03;
    if ((encoding & 1U) && width != 1) return -1;
    uint32_t type  = (encoding >> 10) & 0x03;
    uint32_t index = (encoding >> 1) & 0x1F;
    if (((encoding >> 1) & 0x1FF) >= 32) {
        return -1;
    }
    int slot = static_cast<int>((width << 7) | (type << 5) | index);
    return (slot >= 0 && slot < static_cast<int>(VMCS_FIELD_SLOTS)) ? slot : -1;
}

bool readField(const SoftVMCSInstance *vmcs, uint32_t encoding, uint64_t &outValue) {
    if (!vmcs) {
        return false;
    }
    int idx = fieldEncodingToIndex(encoding);
    if (idx < 0) {
        return false;
    }
    uint64_t val = vmcs->fields[idx];
    uint32_t width = (encoding >> 13) & 0x03;
    if (width == 0) {
        val &= 0xFFFFULL;
    } else if (width == 2) {
        val &= 0xFFFFFFFFULL;
    } else if (width == 1 && (encoding & 1U)) {
        val >>= 32;
    }
    outValue = val;
    return true;
}

bool writeField(SoftVMCSInstance *vmcs, uint32_t encoding, uint64_t value) {
    if (!vmcs) {
        return false;
    }
    int idx = fieldEncodingToIndex(encoding);
    if (idx < 0) {
        return false;
    }
    uint32_t width = (encoding >> 13) & 0x03;
    if (width == 0) {
        vmcs->fields[idx] = value & 0xFFFFULL;
    } else if (width == 2) {
        vmcs->fields[idx] = value & 0xFFFFFFFFULL;
    } else if (width == 1 && (encoding & 1U)) {
        vmcs->fields[idx] = (vmcs->fields[idx] & 0xFFFFFFFFULL) | ((value & 0xFFFFFFFFULL) << 32);
    } else {
        vmcs->fields[idx] = value;
    }
    return true;
}

static SoftVMCSInstance *findSlot(uint64_t pa) {
    for (auto &slot : gVMCSPool) if (slot.inUse && slot.vmcsPhysAddr == pa) return &slot;
    return nullptr;
}
struct LeaseFactory {
    static Lease pin(SoftVMCSInstance *slot) {return Lease(slot);}
};
static Lease leaseSlot(SoftVMCSInstance *slot) {
    if (!slot || __atomic_test_and_set(&slot->leased, __ATOMIC_ACQUIRE)) return Lease{};
    return LeaseFactory::pin(slot);
}
Lease findByPhysAddr(uint64_t vmcsPA) {
    Runtime::Guard guard(gRegistry);
    return guard && gPrepared ? leaseSlot(findSlot(vmcsPA)) : Lease{};
}
static Lease create(uint64_t vmcsPA,uint64_t owner=0,uint64_t lifetime=0,const NPTManager::SpaceKey &space={}) {
    if (!vmcsPA || (vmcsPA & ~0x000ffffffffff000ULL)) return Lease{};
#ifndef AMDHV_KERNEL_BUILD
    init();
#endif
    Runtime::Guard guard(gRegistry);
    if (!guard || !gPrepared) return Lease{};
    if (auto existing = findSlot(vmcsPA)) {
        if(owner && (existing->appleOwner!=owner || existing->appleLifetime!=lifetime ||
           existing->space.owner()!=space.owner() || existing->space.lifetime()!=space.lifetime() || existing->space.eptp()!=space.eptp())) return {};
        return leaseSlot(existing);
    }
    if(gLifetime==UINT64_MAX) return {};
    for (size_t i = 0; i < gSlotLimit; ++i) {
        auto &slot = gVMCSPool[i];
        if (slot.inUse) continue;
        slot.launched = false; slot.vmcsPhysAddr = vmcsPA;
        slot.lastNptGen = slot.lastRoot = 0; slot.lastCPU = MAX_CPUS;
        slot.space=space;slot.appleOwner=owner;slot.appleLifetime=lifetime;slot.guestEferShadow = 0; slot.lifetime = ++gLifetime;
        memset(slot.fields, 0, sizeof(slot.fields));
        memset(slot.vmcbVA, 0, sizeof(VMCB));
        slot.inUse = true;
        return leaseSlot(&slot);
    }
    return Lease{};
}
Lease getOrCreateByPhysAddr(uint64_t vmcsPA) {return create(vmcsPA);}
Lease claimOwned(uint64_t vmcsPA,uint64_t owner,uint64_t lifetime,const NPTManager::SpaceKey &space) {
    if(!owner || !lifetime || !space) return {};
    return create(vmcsPA,owner,lifetime,space);
}
bool releaseByPhysAddr(uint64_t vmcsPA) {
    Runtime::Guard guard(gRegistry);
    if (!guard) return false;
    auto slot = findSlot(vmcsPA);
    if (!slot) return true;
    if (slot->ownerCPU != MAX_CPUS) return false;
    auto lease = leaseSlot(slot);
    if (!lease) return false;
    slot->inUse = false; slot->launched = false; slot->vmcsPhysAddr = 0;
    memset(slot->fields, 0, sizeof(slot->fields));
    return true;
}
Lease getActiveVMCS(uint32_t cpuId) {
    Runtime::Guard guard(gRegistry);
    return guard && cpuId < MAX_CPUS ? leaseSlot(gActiveVMCSPerCPU[cpuId]) : Lease{};
}
bool snapshotActive(uint32_t cpuId,ActiveIdentity &out) {
    out={};Runtime::Guard guard(gRegistry);if(!guard || !gPrepared || cpuId>=MAX_CPUS) return false;
    auto slot=gActiveVMCSPerCPU[cpuId];
    if(slot) out={slot->vmcsPhysAddr,slot->appleOwner,slot->appleLifetime};
    return true;
}
static bool updateBinding(uint32_t cpuId, SoftVMCSInstance *vmcs) {
    Runtime::Guard guard(gRegistry);
    if (!guard || cpuId >= MAX_CPUS) return false;
    if (vmcs) {
        bool found = false;
        for (auto &slot : gVMCSPool) if (&slot == vmcs && slot.inUse) found = true;
        if (!found || (vmcs->ownerCPU != MAX_CPUS && vmcs->ownerCPU != cpuId)) return false;
    }
    auto previous = gActiveVMCSPerCPU[cpuId];
    if (previous && previous != vmcs && __atomic_load_n(&previous->leased, __ATOMIC_ACQUIRE)) return false;
    if (previous) previous->ownerCPU = MAX_CPUS;
    gActiveVMCSPerCPU[cpuId] = vmcs;
    if (vmcs) vmcs->ownerCPU = cpuId;
    return true;
}

bool setActiveVMCS(uint32_t cpuId,const Lease &vmcs) {
    return vmcs && updateBinding(cpuId,vmcs.get());
}
bool clearActiveVMCS(uint32_t cpuId) {return updateBinding(cpuId,nullptr);}

// ============================================================================
// Synthesized Intel VMX & Architectural Capability MSRs
// Offline supported subset; intentionally does not satisfy every AppleHV requirement.
// ============================================================================
bool readSynthesizedMSR(uint32_t msr,uint64_t &out) {
#if defined(AMDHV_KERNEL_BUILD)
    if (!BackendPolicy::executionEnabled) return false;
#endif
    switch(msr) {
        case MSR_IA32_FEATURE_CONTROL: out=5;return true;
        case MSR_IA32_VMX_BASIC: out=1ULL|(0x1000ULL<<32)|(6ULL<<50)|(1ULL<<55);return true;
        case MSR_IA32_VMX_PINBASED_CTLS: case MSR_IA32_VMX_TRUE_PINBASED_CTLS: out=Architecture::capability(Architecture::pin);return true;
        case MSR_IA32_VMX_PROCBASED_CTLS: case MSR_IA32_VMX_TRUE_PROCBASED_CTLS: out=Architecture::capability(Architecture::primary);return true;
        case MSR_IA32_VMX_PROCBASED_CTLS2: out=Architecture::capability(Architecture::secondary);return true;
        case MSR_IA32_VMX_ENTRY_CTLS: case MSR_IA32_VMX_TRUE_ENTRY_CTLS: out=Architecture::capability(Architecture::entry);return true;
        case MSR_IA32_VMX_EXIT_CTLS: case MSR_IA32_VMX_TRUE_EXIT_CTLS: out=Architecture::capability(Architecture::exit);return true;
        case MSR_IA32_VMX_EPT_VPID_CAP:
            out=(1ULL<<6)|(1ULL<<14)|(1ULL<<16)|(1ULL<<17)|(1ULL<<20)|(1ULL<<25)|(1ULL<<26);return true;
        case MSR_IA32_VMX_CR0_FIXED0: out=0x80000021;return true;
        case MSR_IA32_VMX_CR0_FIXED1: out=0xffffffff;return true;
        case MSR_IA32_VMX_CR4_FIXED0: out=0x2000;return true;
        case MSR_IA32_VMX_CR4_FIXED1: out=0x003767ff;return true;
        case MSR_IA32_VMX_VMCS_ENUM: out=0x2e;return true;
        case MSR_IA32_VMX_MISC: case MSR_IA32_VMX_VMFUNC: out=0;return true;
        default: return false;
    }
}
bool writeSynthesizedMSR(uint32_t msr,uint64_t value) {
    (void)msr;(void)value;
    // Capability MSRs are read-only. No fake writes to security/performance MSRs.
    return false;
}

static inline void vmxSucceed(XNUSavedState64 *state) {
    state->rflags &= ~RFLAGS_VMX_MASK;
}

static inline void vmxFailValid(XNUSavedState64 *state) {
    state->rflags = (state->rflags & ~RFLAGS_VMX_MASK) | RFLAGS_ZF;
}

static inline void vmxFailInvalid(XNUSavedState64 *state) {
    state->rflags = (state->rflags & ~RFLAGS_VMX_MASK) | RFLAGS_CF;
}

bool handleKernelTrap(XNUSavedState64 *state) {
    return TrapAdapter::global().handle(state);
}

bool emulateInstruction(XNUSavedState64 *state,const uint8_t *bytes,size_t length,
                        const MemoryAccess::Access &memory,uint32_t cpuId,bool ownedSVMCPU) {
    if (!state || (state->cs&3) || cpuId>=MAX_CPUS ||
        (state->trapno!=XNU_TRAP_INVALID_OPCODE && state->trapno!=XNU_TRAP_GENERAL_PROTECTION)) return false;
    InstructionDecoder::DecodedInstruction inst;
    if (!InstructionDecoder::decodeInstruction(bytes,length,state,inst) ||
        !MemoryAccess::span(state->rip,inst.length)) return false;
    Runtime::Guard admission(gEmulator);
    if (!admission) return false;
#if defined(AMDHV_KERNEL_BUILD)
    if(!ownedSVMCPU) return false;
#endif
    if(ownedSVMCPU && (inst.op==InstructionDecoder::DecodedOp::VMXOn || inst.op==InstructionDecoder::DecodedOp::VMXOff)) return false;
    bool msr=inst.op==InstructionDecoder::DecodedOp::Rdmsr || inst.op==InstructionDecoder::DecodedOp::Wrmsr;
    if (!msr && !ownedSVMCPU && inst.op!=InstructionDecoder::DecodedOp::VMXOn && !gVMXON[cpuId]) return false;
    uint64_t operand=0;
    bool readsOperand=inst.op==InstructionDecoder::DecodedOp::VMWrite || inst.op==InstructionDecoder::DecodedOp::VMClear ||
        inst.op==InstructionDecoder::DecodedOp::VMPtrLd || inst.op==InstructionDecoder::DecodedOp::VMXOn;
    if (readsOperand && inst.isMemOperand && !memory.load(inst.memAddress,&operand,8)) return false;
    auto validRegion=[&](uint64_t pa) {
        uint32_t revision=0;
        return pa && !(pa&~0x000ffffffffff000ULL) && memory.physicalRead &&
            memory.physicalRead(memory.context,pa,&revision,sizeof(revision)) && revision==1;
    };
    switch (inst.op) {
        case InstructionDecoder::DecodedOp::VMRead: {
            // vmread r/m64 (dest: rm), r64 (src encoding: reg)
            uint64_t requestedEncoding=InstructionDecoder::getRegister(state,inst.regIndex);
            uint32_t encoding=static_cast<uint32_t>(requestedEncoding);
            auto vmcs = getActiveVMCS(cpuId);
            uint64_t val = 0;
            if (!vmcs) {
                vmxFailInvalid(state);
            } else if (requestedEncoding!=encoding || !readField(vmcs.get(), encoding, val)) {
                writeField(vmcs.get(), VMCS_RO_INSTR_ERROR, 12);
                vmxFailValid(state);
            } else {
                if (inst.isMemOperand) {
                    if (!memory.store(inst.memAddress,&val,8)) return false;
                } else {
                    InstructionDecoder::setRegister(state, inst.rmRegIndex, val);
                }
                vmxSucceed(state);
            }
            state->rip += inst.length;
            return true;
        }

        case InstructionDecoder::DecodedOp::VMWrite: {
            // vmwrite r64 (dest encoding: reg), r/m64 (src value: rm)
            uint64_t requestedEncoding=InstructionDecoder::getRegister(state,inst.regIndex);
            uint32_t encoding=static_cast<uint32_t>(requestedEncoding);
            uint64_t val = inst.isMemOperand
                ? operand
                : InstructionDecoder::getRegister(state, inst.rmRegIndex);
            auto vmcs = getActiveVMCS(cpuId);
            if (!vmcs) {
                vmxFailInvalid(state);
            } else if (requestedEncoding!=encoding || fieldEncodingToIndex(encoding)<0 || ((encoding >> 10) & 3) == 1 || !writeField(vmcs.get(), encoding, val)) {
                writeField(vmcs.get(), VMCS_RO_INSTR_ERROR, requestedEncoding==encoding && fieldEncodingToIndex(encoding)>=0 && ((encoding >> 10) & 3) == 1 ? 13 : 12);
                vmxFailValid(state);
            } else {
                vmxSucceed(state);
            }
            state->rip += inst.length;
            return true;
        }

        case InstructionDecoder::DecodedOp::VMClear: {
            uint64_t vmcsPA = operand;
            if (vmcsPA==gVMXON[cpuId] || !validRegion(vmcsPA)) { vmxFailInvalid(state); state->rip+=inst.length;return true; }
            auto vmcs = ownedSVMCPU?findByPhysAddr(vmcsPA):getOrCreateByPhysAddr(vmcsPA);
            if (vmcs) {
                if(vmcs->ownerCPU!=MAX_CPUS && vmcs->ownerCPU!=cpuId) {vmxFailInvalid(state);state->rip+=inst.length;return true;}
                vmcs->launched = false;
                if (vmcs->ownerCPU == cpuId) {
                    vmcs.reset();
                    if (!clearActiveVMCS(cpuId)) return false;
                }
                vmxSucceed(state);
            } else {
                vmxFailInvalid(state);
            }
            state->rip += inst.length;
            return true;
        }

        case InstructionDecoder::DecodedOp::VMPtrLd: {
            uint64_t vmcsPA = operand;
            if (vmcsPA==gVMXON[cpuId] || !validRegion(vmcsPA)) { vmxFailInvalid(state); state->rip+=inst.length;return true; }
            auto vmcs = ownedSVMCPU?findByPhysAddr(vmcsPA):getOrCreateByPhysAddr(vmcsPA);
            if (vmcs) {
                if (setActiveVMCS(cpuId, vmcs)) vmxSucceed(state);
                else vmxFailInvalid(state);
            } else {
                vmxFailInvalid(state);
            }
            state->rip += inst.length;
            return true;
        }

        case InstructionDecoder::DecodedOp::VMPtrSt: {
            auto vmcs = getActiveVMCS(cpuId);
            uint64_t value=vmcs ? vmcs->vmcsPhysAddr : UINT64_MAX;
            if (!memory.store(inst.memAddress,&value,8)) return false;
            vmxSucceed(state);
            state->rip += inst.length;
            return true;
        }

        case InstructionDecoder::DecodedOp::VMXOn: {
            if (gVMXON[cpuId] || !getHostSavePA(cpuId) || !validRegion(operand)) vmxFailInvalid(state);
            else { gVMXON[cpuId]=operand;vmxSucceed(state); }
            state->rip+=inst.length;return true;
        }
        case InstructionDecoder::DecodedOp::VMXOff: {
            if (!clearActiveVMCS(cpuId)) return false;
            gVMXON[cpuId]=0;vmxSucceed(state);state->rip+=inst.length;return true;
        }
        case InstructionDecoder::DecodedOp::InvEpt: {
            uint64_t desc[2] {};
            if (!memory.load(inst.memAddress,desc,sizeof(desc))) return false;
            uint64_t type=InstructionDecoder::getRegister(state,inst.regIndex);
            bool valid=(type==1 && !desc[1] && desc[0] && (desc[0]&~NPTManager::PHYS_ADDR_MASK)==0x1e) || type==2;
            if (!valid) vmxFailInvalid(state);
            else if (!(ownedSVMCPU?NPTManager::acknowledgeRevoked(type==2?0:desc[0]):NPTManager::invalidateEPTP(type==2?0:desc[0]))) return false;
            else vmxSucceed(state);
            state->rip+=inst.length;return true;
        }
        case InstructionDecoder::DecodedOp::InvVpid:
            return false; // Not advertised; never pretend an unflushed VPID succeeded.

        case InstructionDecoder::DecodedOp::Rdmsr: {
            uint32_t msrIdx = static_cast<uint32_t>(state->rcx);
            uint64_t val = 0;
            if (readSynthesizedMSR(msrIdx, val)) {
                state->rax = val & 0xFFFFFFFFULL;
                state->rdx = (val >> 32) & 0xFFFFFFFFULL;
                state->rip += inst.length;
                return true;
            }
            return false;
        }

        case InstructionDecoder::DecodedOp::Wrmsr: {
            uint32_t msrIdx = static_cast<uint32_t>(state->rcx);
            uint64_t val = (state->rax & 0xFFFFFFFFULL) | ((state->rdx & 0xFFFFFFFFULL) << 32);
            if (writeSynthesizedMSR(msrIdx, val)) {
                state->rip += inst.length;
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

} // namespace SoftVMCS
