#include <src/VMCBTranslator.hpp>
#include <src/NPTManager.hpp>
#include <src/Architecture.hpp>
#include <src/InstructionDecoder.hpp>
#include <Headers/Lilu/kern_util.hpp>

namespace VMCBTranslator {

uint16_t intelARToAMDAttrib(uint32_t intelAR) {
    // Bit 16 in Intel VMX Access Rights is "Unusable"
    if (intelAR & (1U << 16)) {
        return 0;
    }
    uint16_t low8  = static_cast<uint16_t>(intelAR & 0xFFU);
    uint16_t high4 = static_cast<uint16_t>((intelAR >> 12) & 0x0FU);
    return static_cast<uint16_t>(low8 | (high4 << 8));
}

uint32_t amdAttribToIntelAR(uint16_t amdAttrib) {
    // If Present bit (bit 7) is 0, mark segment as Unusable (bit 16) in Intel VMX format
    if ((amdAttrib & 0x80U) == 0) {
        return (1U << 16);
    }
    uint32_t low8  = static_cast<uint32_t>(amdAttrib & 0xFFU);
    uint32_t high4 = static_cast<uint32_t>((amdAttrib >> 8) & 0x0FU);
    return low8 | (high4 << 12);
}

uint64_t amdIOInfoToIntelIOQual(uint64_t ioExitInfo1) {
    // AMD SVM EXITINFO1 for IOIO:
    // Bit 0:     Type (0 = OUT, 1 = IN)
    // Bit 2:     STR (1 = INS/OUTS string I/O)
    // Bit 3:     REP (1 = REP prefix present)
    // Bit 4:     SZ8 (8-bit operand)
    // Bit 5:     SZ16 (16-bit operand)
    // Bit 6:     SZ32 (32-bit operand)
    // Bits 31:16: Port number
    //
    // Intel VMX Exit Qualification for I/O Instructions (Table 27-5):
    // Bits 2:0:  Size of access (0 = 1-byte, 1 = 2-byte, 3 = 4-byte)
    // Bit 3:     Direction (0 = OUT, 1 = IN)
    // Bit 4:     String instruction (0 = not string, 1 = string)
    // Bit 5:     REP prefixed (0 = no REP, 1 = REP)
    // Bit 6:     Operand encoding (0 = DX, 1 = immediate)
    // Bits 31:16: Port number
    uint64_t sizeCode = 0;
    if (ioExitInfo1 & (1ULL << 5)) {
        sizeCode = 1; // 16-bit
    } else if (ioExitInfo1 & (1ULL << 6)) {
        sizeCode = 3; // 32-bit
    }

    uint64_t dir = (ioExitInfo1 & (1ULL << 0)) ? (1ULL << 3) : 0;
    uint64_t str = (ioExitInfo1 & (1ULL << 2)) ? (1ULL << 4) : 0;
    uint64_t rep = (ioExitInfo1 & (1ULL << 3)) ? (1ULL << 5) : 0;
    uint64_t port = ioExitInfo1 & 0xFFFF0000ULL;

    return sizeCode | dir | str | rep | port;
}

static uint64_t getField(const SoftVMCS::SoftVMCSInstance *vmcs, uint32_t encoding) {
    uint64_t val = 0;
    SoftVMCS::readField(vmcs, encoding, val);
    return val;
}

static void setField(SoftVMCS::SoftVMCSInstance *vmcs, uint32_t encoding, uint64_t val) {
    SoftVMCS::writeField(vmcs, encoding, val);
}

static void syncSegmentToVMCB(const SoftVMCS::SoftVMCSInstance *vmcs, SVMSegment &seg,
                              uint32_t selEnc, uint32_t limitEnc, uint32_t arEnc, uint32_t baseEnc) {
    seg.selector = static_cast<uint16_t>(getField(vmcs, selEnc));
    seg.limit    = static_cast<uint32_t>(getField(vmcs, limitEnc));
    seg.attrib   = intelARToAMDAttrib(static_cast<uint32_t>(getField(vmcs, arEnc)));
    seg.base     = getField(vmcs, baseEnc);
}

static void syncSegmentFromVMCB(SoftVMCS::SoftVMCSInstance *vmcs, const SVMSegment &seg,
                                uint32_t selEnc, uint32_t limitEnc, uint32_t arEnc, uint32_t baseEnc) {
    setField(vmcs, selEnc,   seg.selector);
    setField(vmcs, limitEnc, seg.limit);
    setField(vmcs, arEnc,    amdAttribToIntelAR(seg.attrib));
    setField(vmcs, baseEnc,  seg.base);
}

static bool validMSRs(const GuestMSRs *msrs) {
    if(!msrs) return true;
    if(msrs->count>32) return false;
    for(size_t i=0;i<msrs->count;++i) {
        const auto &entry=msrs->entries[i];
        if(entry.reserved || (entry.index!=MSR_IA32_STAR && entry.index!=MSR_IA32_LSTAR && entry.index!=MSR_IA32_CSTAR &&
           entry.index!=MSR_IA32_FMASK && entry.index!=MSR_IA32_KERNEL_GS_BASE)) return false;
        if((entry.index==MSR_IA32_LSTAR || entry.index==MSR_IA32_CSTAR || entry.index==MSR_IA32_KERNEL_GS_BASE) &&
           !MemoryAccess::canonical(entry.data)) return false;
        if(entry.index==MSR_IA32_FMASK && entry.data>UINT32_MAX) return false;
        for(size_t j=0;j<i;++j) if(msrs->entries[j].index==entry.index) return false;
    }
    return true;
}

EntryResult syncVMCStoVMCB(SoftVMCS::SoftVMCSInstance *vmcs, AppleHVGPRFrame *gprs, GuestMSRs *msrs, const NPTManager::Lease *root) {
    if (!vmcs || !vmcs->vmcbVA) {
        return EntryResult::InvalidArgument;
    }

    auto field=[&](uint32_t encoding) {return getField(vmcs,encoding);};
    if ((field(VMCS_CTRL_PIN_BASED)&~Architecture::pin) ||
        (field(VMCS_CTRL_CPU_BASED)&~Architecture::primary) ||
        (field(VMCS_CTRL_CPU_BASED2)&~Architecture::secondary) ||
        (field(VMCS_CTRL_VMENTRY_CONTROLS)&~Architecture::entry) ||
        (field(VMCS_CTRL_VMEXIT_CONTROLS)&~Architecture::exit) ||
        field(VMCS_CTRL_VMEXIT_MSR_STORE_COUNT) || field(VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT) ||
        field(VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT) || field(VMCS_CTRL_CR0_MASK) || field(VMCS_CTRL_CR4_MASK) ||
        field(VMCS_CTRL_CR3_COUNT) || field(VMCS_GUEST_ACTIVITY_STATE) || field(VMCS_GUEST_DEBUG_EXC) ||
        field(VMCS_GUEST_IA32_DEBUGCTL) || field(VMCS_GUEST_IGNORE_IRQ) ||
        !(field(VMCS_CTRL_CPU_BASED)&CPU_BASED_UNCOND_IO_EXIT)) return EntryResult::UnsupportedControls;
    uint64_t event=0;
    if (!Architecture::toSVMEvent(uint32_t(field(VMCS_CTRL_VMENTRY_IRQ_INFO)),uint32_t(field(VMCS_CTRL_VMENTRY_EXC_ERROR)),event)) return EntryResult::InvalidEvent;
    if(!validMSRs(msrs)) return EntryResult::InvalidMSRs;
    VMCB *vmcb = vmcs->vmcbVA;
    VMCBControlArea   &ctrl  = vmcb->control;
    VMCBStateSaveArea &state = vmcb->state;

    // 1. Sync Guest Segments & Descriptor Tables
    syncSegmentToVMCB(vmcs, state.es,   VMCS_GUEST_ES,   VMCS_GUEST_ES_LIMIT,   VMCS_GUEST_ES_AR,   VMCS_GUEST_ES_BASE);
    syncSegmentToVMCB(vmcs, state.cs,   VMCS_GUEST_CS,   VMCS_GUEST_CS_LIMIT,   VMCS_GUEST_CS_AR,   VMCS_GUEST_CS_BASE);
    syncSegmentToVMCB(vmcs, state.ss,   VMCS_GUEST_SS,   VMCS_GUEST_SS_LIMIT,   VMCS_GUEST_SS_AR,   VMCS_GUEST_SS_BASE);
    syncSegmentToVMCB(vmcs, state.ds,   VMCS_GUEST_DS,   VMCS_GUEST_DS_LIMIT,   VMCS_GUEST_DS_AR,   VMCS_GUEST_DS_BASE);
    syncSegmentToVMCB(vmcs, state.fs,   VMCS_GUEST_FS,   VMCS_GUEST_FS_LIMIT,   VMCS_GUEST_FS_AR,   VMCS_GUEST_FS_BASE);
    syncSegmentToVMCB(vmcs, state.gs,   VMCS_GUEST_GS,   VMCS_GUEST_GS_LIMIT,   VMCS_GUEST_GS_AR,   VMCS_GUEST_GS_BASE);
    syncSegmentToVMCB(vmcs, state.ldtr, VMCS_GUEST_LDTR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, VMCS_GUEST_LDTR_BASE);
    syncSegmentToVMCB(vmcs, state.tr,   VMCS_GUEST_TR,   VMCS_GUEST_TR_LIMIT,   VMCS_GUEST_TR_AR,   VMCS_GUEST_TR_BASE);

    state.gdtr.base  = getField(vmcs, VMCS_GUEST_GDTR_BASE);
    state.gdtr.limit = static_cast<uint32_t>(getField(vmcs, VMCS_GUEST_GDTR_LIMIT));
    state.idtr.base  = getField(vmcs, VMCS_GUEST_IDTR_BASE);
    state.idtr.limit = static_cast<uint32_t>(getField(vmcs, VMCS_GUEST_IDTR_LIMIT));

    // 2. Sync Control Registers, EFER, RFLAGS, RIP, RSP, RAX, CR2
    uint64_t cr0 = getField(vmcs, VMCS_GUEST_CR0);
    uint64_t cr3 = getField(vmcs, VMCS_GUEST_CR3);
    uint64_t cr4 = getField(vmcs, VMCS_GUEST_CR4);
    // Clear Intel-specific VMXE bit (bit 13) from guest CR4 on AMD hardware
    state.cr0 = cr0;
    state.cr3 = cr3;
    state.cr4 = cr4 & ~(1ULL << 13);
    state.dr7 = getField(vmcs, VMCS_GUEST_DR7);

    uint64_t entryCtls = getField(vmcs, VMCS_CTRL_VMENTRY_CONTROLS);
    uint64_t guestEfer = getField(vmcs, VMCS_GUEST_IA32_EFER);
    if (entryCtls & VMENTRY_IA32E_MODE_GUEST) {
        guestEfer |= (EFER_LME | EFER_LMA);
    }
    vmcs->guestEferShadow = guestEfer;
    // AMD SVM hardware strictly requires EFER.SVME (bit 12) == 1 in VMCB state.efer
    state.efer = guestEfer | EFER_SVME;

    state.rflags = getField(vmcs, VMCS_GUEST_RFLAGS);
    state.rip    = getField(vmcs, VMCS_GUEST_RIP);
    state.rsp    = getField(vmcs, VMCS_GUEST_RSP);

    // Derive CPL: V8086 mode -> 3, Real mode -> 0, Protected/Long mode -> CS.DPL (or SS.DPL)
    if (state.rflags & (1ULL << 17)) {
        state.cpl = 3;
    } else if ((cr0 & 1ULL) == 0) {
        state.cpl = 0;
    } else {
        state.cpl = static_cast<uint8_t>((state.ss.attrib >> 5) & 0x03);
    }

    state.sysenter_cs  = getField(vmcs, VMCS_GUEST_IA32_SYSENTER_CS);
    state.sysenter_esp = getField(vmcs, VMCS_GUEST_SYSENTER_ESP);
    state.sysenter_eip = getField(vmcs, VMCS_GUEST_SYSENTER_EIP);
    state.dbgctl       = getField(vmcs, VMCS_GUEST_IA32_DEBUGCTL);

    uint64_t pat = getField(vmcs, VMCS_GUEST_IA32_PAT);
    state.g_pat  = pat;

    if (gprs) {
        state.rax    = gprs->rax;
        state.cr2    = gprs->cr2;
        gprs->rip    = state.rip;
        gprs->rflags = state.rflags;
        gprs->rsp    = state.rsp;
    }

    if (msrs) for(size_t i=0;i<msrs->count;++i) {
        const auto &entry=msrs->entries[i];
        switch(entry.index) {
            case MSR_IA32_STAR:state.star=entry.data;break;
            case MSR_IA32_LSTAR:state.lstar=entry.data;break;
            case MSR_IA32_CSTAR:state.cstar=entry.data;break;
            case MSR_IA32_FMASK:state.sfmask=entry.data;break;
            case MSR_IA32_KERNEL_GS_BASE:state.kernel_gs_base=entry.data;break;
        }
    }

    // 3. Interrupt Shadow (Intel VMCS_GUEST_IGNORE_IRQ bit 0=STI, bit 1=MOV SS -> AMD interrupt_shadow bit 0)
    uint64_t ignoreIrq = getField(vmcs, VMCS_GUEST_IGNORE_IRQ);
    ctrl.interrupt_shadow = (ignoreIrq & 0x3ULL) ? 1ULL : 0ULL;

    // 4. Translate Control Fields & Intercepts
    uint32_t procCtls  = static_cast<uint32_t>(getField(vmcs, VMCS_CTRL_CPU_BASED));
    uint32_t procCtls2 = (procCtls & CPU_BASED_ACTIVATE_SEC_CTLS)
        ? static_cast<uint32_t>(getField(vmcs, VMCS_CTRL_CPU_BASED2))
        : 0;

    uint16_t crRead  = 0;
    uint16_t crWrite = 0;
    uint16_t drRead  = 0;
    uint16_t drWrite = 0;
    uint32_t ops1    = SVM_INTERCEPT_INTR | SVM_INTERCEPT_NMI | SVM_INTERCEPT_SMI | SVM_INTERCEPT_INIT |
                       SVM_INTERCEPT_CPUID | SVM_INTERCEPT_INVD | SVM_INTERCEPT_INVLPGA |
                       SVM_INTERCEPT_IOIO_PROT | SVM_INTERCEPT_MSR_PROT |
                       SVM_INTERCEPT_SHUTDOWN | SVM_INTERCEPT_FERR_FREEZE | SVM_INTERCEPT_RSM;
    uint32_t ops2    = SVM_INTERCEPT_VMRUN | SVM_INTERCEPT_VMMCALL | SVM_INTERCEPT_VMLOAD |
                       SVM_INTERCEPT_VMSAVE | SVM_INTERCEPT_STGI | SVM_INTERCEPT_CLGI |
                       SVM_INTERCEPT_SKINIT | SVM_INTERCEPT_XSETBV;
    uint32_t ops3    = 0;

    if (procCtls & CPU_BASED_HLT_EXIT)      ops1 |= SVM_INTERCEPT_HLT;
    if (procCtls & CPU_BASED_RDPMC_EXIT)    ops1 |= SVM_INTERCEPT_RDPMC;
    if (procCtls & CPU_BASED_RDTSC_EXIT)    ops1 |= SVM_INTERCEPT_RDTSC;
    if (procCtls & CPU_BASED_PAUSE_EXIT)    ops1 |= SVM_INTERCEPT_PAUSE;
    if (procCtls2 & CPU_BASED2_WBINVD_EXIT) {
        ops2 |= SVM_INTERCEPT_WBINVD;
    }
    if ((procCtls2 & CPU_BASED2_ENABLE_RDTSCP) == 0) {
        ops2 |= SVM_INTERCEPT_RDTSCP;
    }
    // Window, virtual-APIC and masked-CR controls are rejected at validation.
    uint64_t vintr=SVM_VINTR_INTR_MASKING;

    ctrl.intercept_cr_read   = crRead;
    ctrl.intercept_cr_write  = crWrite;
    ctrl.intercept_dr_read   = drRead;
    ctrl.intercept_dr_write  = drWrite;
    ctrl.intercept_exception = static_cast<uint32_t>(getField(vmcs, VMCS_CTRL_EXC_BITMAP));
    ctrl.intercept_ops1      = ops1;
    ctrl.intercept_ops2      = ops2;
    ctrl.intercept_ops3      = ops3;
    ctrl.vintr               = vintr;

    ctrl.iopm_base_pa  = vmcs->iopmPA;
    ctrl.msrpm_base_pa = vmcs->msrpmPA;
    ctrl.tsc_offset    = (procCtls & CPU_BASED_USE_TSC_OFFSET) ? getField(vmcs, VMCS_CTRL_TSC_OFFSET) : 0;
    ctrl.guest_asid    = vmcs->asid;

    ctrl.event_inj=event;
    vmcs->preparedEvent=uint32_t(field(VMCS_CTRL_VMENTRY_IRQ_INFO));
    // Pending injection is consumed only after a confirmed successful entry.

    // Nested paging is mandatory; no failure may fall back to host physical RAM.
    uint64_t eptp = getField(vmcs, VMCS_CTRL_EPTP), nptGen=0, ncr3=0;
    if ((procCtls2 & CPU_BASED2_ENABLE_EPT) && eptp) {
        if (root && *root && vmcs->space && vmcs->space.eptp()==eptp && root->matches(vmcs->space)) {ncr3=root->root();nptGen=root->generation();}
#ifndef AMDHV_KERNEL_BUILD
        else ncr3=NPTManager::getOrCreateNCR3(eptp,nptGen);
#endif
    }
    ctrl.np_enable=1;
    ctrl.n_cr3=ncr3;
    // Conservative flush-all works without optional flush-by-ASID support and
    // covers CPU migration, recycled ASIDs and changed roots on every entry.
    ctrl.tlb_control=1;
    ctrl.vmcb_clean_bits=0;
    vmcs->lastNptGen=nptGen;
    return ncr3 && vmcs->asid && vmcs->iopmPA && vmcs->msrpmPA ? EntryResult::Ready : EntryResult::MissingNPT;
}

static uint32_t computeInstructionLength(const VMCB *vmcb) {
    if (vmcb->control.next_rip > vmcb->state.rip) {
        uint64_t diff = vmcb->control.next_rip - vmcb->state.rip;
        if (diff > 0 && diff <= 15) {
            return static_cast<uint32_t>(diff);
        }
    }
    if (vmcb->control.num_bytes_fetched > 0) {
        return InstructionDecoder::decodeInstructionLength64(
            vmcb->control.guest_inst_bytes,
            vmcb->control.num_bytes_fetched <= 15 ? vmcb->control.num_bytes_fetched : 15);
    }
    return 0; // Unknown length is not permission to advance the guest.
}

ExitResult syncVMCBtoVMCS(SoftVMCS::SoftVMCSInstance *vmcs, AppleHVGPRFrame *gprs, GuestMSRs *msrs, bool &outFastPathRetry) {
    outFastPathRetry = false;
    if (!vmcs || !vmcs->vmcbVA) {
        return ExitResult::InvalidArgument;
    }

    const VMCB *vmcb = vmcs->vmcbVA;
    const VMCBControlArea   &ctrl  = vmcb->control;
    const VMCBStateSaveArea &state = vmcb->state;

    // Clear exit-only metadata before any failure return; callers must never
    // mistake an earlier successful exit's fields for this failed translation.
    setField(vmcs,VMCS_RO_EXIT_REASON,0xffffffffU);
    const uint32_t exitFields[]={uint32_t(VMCS_RO_EXIT_QUALIFIC),uint32_t(VMCS_RO_VMEXIT_IRQ_INFO),
        uint32_t(VMCS_RO_VMEXIT_IRQ_ERROR),uint32_t(VMCS_RO_VMEXIT_INSTR_LEN),
        uint32_t(VMCS_RO_VMEXIT_INSTR_INFO),uint32_t(VMCS_RO_GUEST_LIN_ADDR),
        uint32_t(VMCS_RO_IDT_VECTOR_INFO),uint32_t(VMCS_RO_IDT_VECTOR_ERROR),
        uint32_t(VMCS_RO_IO_RCX),uint32_t(VMCS_RO_IO_RSI),uint32_t(VMCS_RO_IO_RDI),uint32_t(VMCS_RO_IO_RIP),
        uint32_t(VMCS_GUEST_PHYSICAL_ADDRESS)};
    for(auto field:exitFields) setField(vmcs,field,0);
    if (ctrl.exit_code==SVM_VMEXIT_INVALID) {
        setField(vmcs,VMCS_RO_EXIT_REASON,(1U<<31)|VMX_REASON_VMENTRY_GUEST);
        setField(vmcs,VMCS_RO_VMEXIT_INSTR_LEN,0);
        setField(vmcs,VMCS_RO_EXIT_QUALIFIC,0);
        return ExitResult::FailedEntry;
    }
    uint32_t vectorInfo=0,vectorError=0;
    if (!Architecture::fromSVMEvent(ctrl.exit_int_info,vectorInfo,vectorError)) return ExitResult::InvalidEvent;
    if(!validMSRs(msrs)) return ExitResult::InvalidArgument;
    if(vmcs->preparedEvent&(1U<<31)) {
        setField(vmcs,VMCS_CTRL_VMENTRY_IRQ_INFO,vectorInfo);
        setField(vmcs,VMCS_CTRL_VMENTRY_EXC_ERROR,vectorError);
    }
    vmcs->preparedEvent=0;
    // 2. Sync Guest Segments & Descriptor Tables back to SoftVMCS
    syncSegmentFromVMCB(vmcs, state.es,   VMCS_GUEST_ES,   VMCS_GUEST_ES_LIMIT,   VMCS_GUEST_ES_AR,   VMCS_GUEST_ES_BASE);
    syncSegmentFromVMCB(vmcs, state.cs,   VMCS_GUEST_CS,   VMCS_GUEST_CS_LIMIT,   VMCS_GUEST_CS_AR,   VMCS_GUEST_CS_BASE);
    syncSegmentFromVMCB(vmcs, state.ss,   VMCS_GUEST_SS,   VMCS_GUEST_SS_LIMIT,   VMCS_GUEST_SS_AR,   VMCS_GUEST_SS_BASE);
    syncSegmentFromVMCB(vmcs, state.ds,   VMCS_GUEST_DS,   VMCS_GUEST_DS_LIMIT,   VMCS_GUEST_DS_AR,   VMCS_GUEST_DS_BASE);
    syncSegmentFromVMCB(vmcs, state.fs,   VMCS_GUEST_FS,   VMCS_GUEST_FS_LIMIT,   VMCS_GUEST_FS_AR,   VMCS_GUEST_FS_BASE);
    syncSegmentFromVMCB(vmcs, state.gs,   VMCS_GUEST_GS,   VMCS_GUEST_GS_LIMIT,   VMCS_GUEST_GS_AR,   VMCS_GUEST_GS_BASE);
    syncSegmentFromVMCB(vmcs, state.ldtr, VMCS_GUEST_LDTR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, VMCS_GUEST_LDTR_BASE);
    syncSegmentFromVMCB(vmcs, state.tr,   VMCS_GUEST_TR,   VMCS_GUEST_TR_LIMIT,   VMCS_GUEST_TR_AR,   VMCS_GUEST_TR_BASE);

    setField(vmcs, VMCS_GUEST_GDTR_BASE,  state.gdtr.base);
    setField(vmcs, VMCS_GUEST_GDTR_LIMIT, state.gdtr.limit);
    setField(vmcs, VMCS_GUEST_IDTR_BASE,  state.idtr.base);
    setField(vmcs, VMCS_GUEST_IDTR_LIMIT, state.idtr.limit);

    // 3. Sync Control Registers, EFER, RIP, RSP, RFLAGS, RAX, CR2
    setField(vmcs, VMCS_GUEST_CR0, state.cr0);
    setField(vmcs, VMCS_GUEST_CR3, state.cr3);
    // Preserve guest's logical CR4.VMXE bit (bit 13) if it was set in SoftVMCS
    uint64_t prevCr4 = getField(vmcs, VMCS_GUEST_CR4);
    setField(vmcs, VMCS_GUEST_CR4, (state.cr4 & ~(1ULL << 13)) | (prevCr4 & (1ULL << 13)));
    setField(vmcs, VMCS_GUEST_DR7, state.dr7);

    // Strip hardware-forced EFER.SVME bit from guest EFER
    uint64_t logicalEfer = (state.efer & ~EFER_SVME) | (vmcs->guestEferShadow & EFER_SVME);
    setField(vmcs, VMCS_GUEST_IA32_EFER, logicalEfer);

    uint64_t entryCtls = getField(vmcs, VMCS_CTRL_VMENTRY_CONTROLS);
    if (logicalEfer & EFER_LMA) {
        entryCtls |= VMENTRY_IA32E_MODE_GUEST;
    } else {
        entryCtls &= ~VMENTRY_IA32E_MODE_GUEST;
    }
    setField(vmcs, VMCS_CTRL_VMENTRY_CONTROLS, entryCtls);

    setField(vmcs, VMCS_GUEST_RIP,    state.rip);
    setField(vmcs, VMCS_GUEST_RSP,    state.rsp);
    setField(vmcs, VMCS_GUEST_RFLAGS, state.rflags);

    setField(vmcs, VMCS_GUEST_IA32_SYSENTER_CS, state.sysenter_cs);
    setField(vmcs, VMCS_GUEST_SYSENTER_ESP,     state.sysenter_esp);
    setField(vmcs, VMCS_GUEST_SYSENTER_EIP,     state.sysenter_eip);
    setField(vmcs, VMCS_GUEST_IA32_PAT,         state.g_pat);
    setField(vmcs, VMCS_GUEST_IA32_DEBUGCTL,    state.dbgctl);
    setField(vmcs, VMCS_GUEST_IGNORE_IRQ,       (ctrl.interrupt_shadow & 1ULL) ? 1ULL : 0ULL);

    if (gprs) {
        gprs->rax    = state.rax;
        gprs->cr2    = state.cr2;
        gprs->rip    = state.rip;
        gprs->rflags = state.rflags;
        gprs->rsp    = state.rsp;
    }

    if (msrs) for(size_t i=0;i<msrs->count;++i) {
        auto &entry=msrs->entries[i];
        switch(entry.index) {
            case MSR_IA32_STAR:entry.data=state.star;break;
            case MSR_IA32_LSTAR:entry.data=state.lstar;break;
            case MSR_IA32_CSTAR:entry.data=state.cstar;break;
            case MSR_IA32_FMASK:entry.data=state.sfmask;break;
            case MSR_IA32_KERNEL_GS_BASE:entry.data=state.kernel_gs_base;break;
        }
    }

    // 4. Sync IDT Vectoring Information (EXITINTINFO -> VMCS_RO_IDT_VECTOR_INFO / ERROR)
    setField(vmcs,VMCS_RO_IDT_VECTOR_INFO,vectorInfo);
    setField(vmcs,VMCS_RO_IDT_VECTOR_ERROR,vectorError);

    // Resolve an NPF once, after saving guest progress. Manager failures are
    // backend failures, not fabricated guest EPT violations.
    uint64_t npfLeaf=0;
    if(ctrl.exit_code==SVM_VMEXIT_NPF) {
        auto result=vmcs->space?NPTManager::synchronize(vmcs->space,ctrl.exit_info_2,&npfLeaf):
            NPTManager::synchronize(getField(vmcs,VMCS_CTRL_EPTP),ctrl.exit_info_2,&npfLeaf);
        if(result!=NPTManager::SyncResult::Synced && result!=NPTManager::SyncResult::NotMapped)
            return ExitResult::MemoryFailure;
        if(result==NPTManager::SyncResult::Synced && !(ctrl.exit_info_1&SVM_NPF_INFO1_PRESENT)) {
            bool write=ctrl.exit_info_1&SVM_NPF_INFO1_WRITE,fetch=ctrl.exit_info_1&SVM_NPF_INFO1_FETCH;
            if((!write || (npfLeaf&NPTManager::EPT_WRITE)) && (!fetch || (npfLeaf&NPTManager::EPT_EXEC))) {
                outFastPathRetry=true;return ExitResult::Retry;
            }
        }
    }

    uint32_t intelReason=0,irqInfo=0,irqError=0;
    uint64_t intelQual=0;
    uint32_t instrLen=computeInstructionLength(vmcb);
    uint64_t exitCode = ctrl.exit_code;

    if (exitCode<=SVM_VMEXIT_DR15_WRITE) {
        setField(vmcs,VMCS_RO_EXIT_REASON,0xffffffffU);
        return ExitResult::UnsupportedExit; // Requires validated decode-assist metadata.
    } else if (exitCode >= SVM_VMEXIT_EXCP_BASE && exitCode <= SVM_VMEXIT_EXCP_LAST) {
        intelReason = VMX_REASON_EXC_NMI;
        uint32_t vec = static_cast<uint32_t>(exitCode - SVM_VMEXIT_EXCP_BASE);
        if (!Architecture::knownException(vec) && vec!=2) return ExitResult::UnsupportedExit;
        if (vec==1 || vec==3 || vec==4) return ExitResult::UnsupportedExit;
        uint32_t type = 3; // Hardware exception
        if (vec == 2) {
            type = 2; // NMI
        }
        bool hasErrCode = (vec == 8 || (vec >= 10 && vec <= 14) || vec == 17 || vec == 21);
        irqInfo  = vec | (type << 8) | (hasErrCode ? (1U << 11) : 0U) | (1U << 31);
        irqError = hasErrCode ? static_cast<uint32_t>(ctrl.exit_info_1) : 0U;
        if (vec == 14) {
            // #PF stores faulting linear address in EXITINFO2 and Exit Qualification
            intelQual = ctrl.exit_info_2;
            if (gprs) gprs->cr2 = ctrl.exit_info_2;
        }
    } else {
        switch (exitCode) {
            case SVM_VMEXIT_INTR:
                if(!(getField(vmcs,VMCS_CTRL_PIN_BASED)&PIN_BASED_INTR_EXIT)) return ExitResult::HostEvent;
                intelReason = VMX_REASON_EXT_IRQ;
                break;
            case SVM_VMEXIT_NMI:
                if(!(getField(vmcs,VMCS_CTRL_PIN_BASED)&PIN_BASED_NMI_EXIT)) return ExitResult::HostEvent;
                intelReason = VMX_REASON_EXC_NMI;
                irqInfo     = 2U | (2U << 8) | (1U << 31);
                break;
            case SVM_VMEXIT_SMI:case SVM_VMEXIT_INIT:
                return ExitResult::HostEvent;
            case SVM_VMEXIT_VINTR: case SVM_VMEXIT_IRET:
                return ExitResult::UnsupportedExit;
            case SVM_VMEXIT_CPUID:
                intelReason = VMX_REASON_CPUID;
                break;
            case SVM_VMEXIT_HLT:
                intelReason = VMX_REASON_HLT;
                break;
            case SVM_VMEXIT_INVD:
                intelReason = VMX_REASON_INVD;
                break;
            case SVM_VMEXIT_INVLPG:
                intelReason = VMX_REASON_INVLPG;
                intelQual   = ctrl.exit_info_1;
                break;
            case SVM_VMEXIT_RDPMC:
                intelReason = VMX_REASON_RDPMC;
                break;
            case SVM_VMEXIT_RDTSC:
                intelReason = VMX_REASON_RDTSC;
                break;
            case SVM_VMEXIT_RDTSCP:
                intelReason = VMX_REASON_RDTSCP;
                break;
            case SVM_VMEXIT_RSM:
                intelReason = VMX_REASON_RSM;
                break;
            case SVM_VMEXIT_VMMCALL:
                intelReason = VMX_REASON_VMCALL;
                break;
            case SVM_VMEXIT_VMRUN: case SVM_VMEXIT_VMLOAD: case SVM_VMEXIT_VMSAVE:
                return ExitResult::UnsupportedExit;
            case SVM_VMEXIT_IOIO: {
                uint64_t sizeBits=ctrl.exit_info_1&0x70;
                if(sizeBits!=0x10 && sizeBits!=0x20 && sizeBits!=0x40) return ExitResult::UnsupportedExit;
                size_t n=ctrl.num_bytes_fetched;
                if(!n || n>15) return ExitResult::UnsupportedExit;
                size_t offset=0;
                bool operandOverride=false,repeat=false;
                while(offset<n && (ctrl.guest_inst_bytes[offset]==0x66 || ctrl.guest_inst_bytes[offset]==0xf3)) {
                    operandOverride|=ctrl.guest_inst_bytes[offset]==0x66;repeat|=ctrl.guest_inst_bytes[offset]==0xf3;++offset;
                }
                if(offset==n) return ExitResult::UnsupportedExit;
                uint8_t opcode=ctrl.guest_inst_bytes[offset];
                bool immediate=opcode>=0xe4 && opcode<=0xe7;
                bool dx=opcode>=0xec && opcode<=0xef;
                bool string=opcode>=0x6c && opcode<=0x6f;
                if((!immediate && !dx && !string) || (immediate && offset+1>=n) ||
                    string!=bool(ctrl.exit_info_1&(1ULL<<2))) return ExitResult::UnsupportedExit;
                bool input=string?opcode<0x6e:((opcode&2)==0);
                bool byte=(opcode&1)==0;
                bool default32=(state.cs.attrib&((1U<<9)|(1U<<10)))!=0;
                uint64_t expectedSize=byte?0x10:(default32!=operandOverride?0x40:0x20);
                if(input!=bool(ctrl.exit_info_1&1) || byte!=(sizeBits==0x10) ||
                   sizeBits!=expectedSize || bool(ctrl.exit_info_1&(1ULL<<3))!=(string && repeat) ||
                   (immediate && uint16_t(ctrl.exit_info_1>>16)!=ctrl.guest_inst_bytes[offset+1])) return ExitResult::UnsupportedExit;
                uint32_t decodedLength=uint32_t(offset+1+(immediate?1:0));
                if(ctrl.next_rip && (ctrl.next_rip<=state.rip || ctrl.next_rip-state.rip!=decodedLength)) return ExitResult::UnsupportedExit;
                instrLen=decodedLength;
                intelReason = VMX_REASON_IO;
                intelQual = amdIOInfoToIntelIOQual(ctrl.exit_info_1)|(immediate?1ULL<<6:0);
                if(gprs && string) {
                    setField(vmcs,VMCS_RO_IO_RCX,gprs->rcx);setField(vmcs,VMCS_RO_IO_RSI,gprs->rsi);
                    setField(vmcs,VMCS_RO_IO_RDI,gprs->rdi);setField(vmcs,VMCS_RO_IO_RIP,state.rip);
                }
                if (ctrl.exit_info_2 > state.rip && (ctrl.exit_info_2 - state.rip) <= 15) {
                    if(ctrl.exit_info_2-state.rip!=decodedLength) return ExitResult::UnsupportedExit;
                }
                break;
            }
            case SVM_VMEXIT_MSR:
                if(ctrl.exit_info_1>1) return ExitResult::UnsupportedExit;
                intelReason = (ctrl.exit_info_1 == 1ULL) ? VMX_REASON_WRMSR : VMX_REASON_RDMSR;
                break;
            case SVM_VMEXIT_SHUTDOWN:
                intelReason = VMX_REASON_TRIPLE_FAULT;
                break;
            case SVM_VMEXIT_WBINVD:
                intelReason = VMX_REASON_WBINVD;
                break;
            case SVM_VMEXIT_MONITOR:
                intelReason = VMX_REASON_MONITOR;
                break;
            case SVM_VMEXIT_MWAIT:
            case SVM_VMEXIT_MWAIT_COND:
                intelReason = VMX_REASON_MWAIT;
                break;
            case SVM_VMEXIT_PAUSE:
                intelReason = VMX_REASON_PAUSE;
                break;
            case SVM_VMEXIT_XSETBV:
                intelReason = VMX_REASON_XSETBV;
                break;
            case SVM_VMEXIT_INVPCID:
                intelReason = VMX_REASON_INVPCID;
                break;
            case SVM_VMEXIT_TASK_SWITCH:
            case SVM_VMEXIT_IDTR_READ: case SVM_VMEXIT_GDTR_READ:
            case SVM_VMEXIT_IDTR_WRITE: case SVM_VMEXIT_GDTR_WRITE:
            case SVM_VMEXIT_LDTR_READ: case SVM_VMEXIT_TR_READ:
            case SVM_VMEXIT_LDTR_WRITE: case SVM_VMEXIT_TR_WRITE:
                return ExitResult::UnsupportedExit;
            case SVM_VMEXIT_NPF: {
                intelReason = VMX_REASON_EPT_VIOLATION;
                intelQual = NPTManager::convertNPFInfoToEPTQualification(ctrl.exit_info_1, npfLeaf);
                setField(vmcs, VMCS_GUEST_PHYSICAL_ADDRESS, ctrl.exit_info_2);
                setField(vmcs, VMCS_RO_GUEST_LIN_ADDR,      0);
                break;
            }
            case SVM_VMEXIT_INVALID:
                intelReason = (1U << 31) | VMX_REASON_VMENTRY_GUEST;
                break;
            default:
                setField(vmcs,VMCS_RO_EXIT_REASON,0xffffffffU);
                return ExitResult::UnsupportedExit;
        }
    }

    bool asynchronous=exitCode==SVM_VMEXIT_NPF || exitCode==SVM_VMEXIT_INTR || exitCode==SVM_VMEXIT_NMI ||
        exitCode==SVM_VMEXIT_VINTR || exitCode==SVM_VMEXIT_SHUTDOWN ||
        (exitCode>=SVM_VMEXIT_EXCP_BASE && exitCode<=SVM_VMEXIT_EXCP_LAST);
    if(asynchronous) instrLen=0;
    else if(!instrLen) return ExitResult::UnsupportedExit;
    setField(vmcs, VMCS_RO_EXIT_REASON,       intelReason);
    setField(vmcs, VMCS_RO_EXIT_QUALIFIC,     intelQual);
    setField(vmcs, VMCS_RO_VMEXIT_IRQ_INFO,   irqInfo);
    setField(vmcs, VMCS_RO_VMEXIT_IRQ_ERROR,  irqError);
    setField(vmcs, VMCS_RO_VMEXIT_INSTR_LEN,  instrLen);
    setField(vmcs, VMCS_RO_VMEXIT_INSTR_INFO, 0);
    return ExitResult::GuestExit;
}

} // namespace VMCBTranslator
