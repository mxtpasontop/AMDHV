#ifndef AMDHV_AMD_SVM_HPP
#define AMDHV_AMD_SVM_HPP

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// AMD SVM Architecture MSRs & Bits
// ============================================================================
static constexpr uint32_t MSR_AMD_EFER          = 0xC0000080;
static constexpr uint64_t EFER_SCE              = (1ULL << 0);
static constexpr uint64_t EFER_LME              = (1ULL << 8);
static constexpr uint64_t EFER_LMA              = (1ULL << 10);
static constexpr uint64_t EFER_NXE              = (1ULL << 11);
static constexpr uint64_t EFER_SVME             = (1ULL << 12);
static constexpr uint64_t EFER_LMSLE            = (1ULL << 13);
static constexpr uint64_t EFER_FFXSR            = (1ULL << 14);
static constexpr uint64_t EFER_TCE              = (1ULL << 15);

static constexpr uint32_t MSR_AMD_VM_CR         = 0xC0010114;
static constexpr uint64_t VM_CR_DPD             = (1ULL << 0);
static constexpr uint64_t VM_CR_R_INIT          = (1ULL << 1);
static constexpr uint64_t VM_CR_DIS_A20M        = (1ULL << 2);
static constexpr uint64_t VM_CR_LOCK            = (1ULL << 3);
static constexpr uint64_t VM_CR_SVMDIS          = (1ULL << 4);

static constexpr uint32_t MSR_AMD_IGNNE         = 0xC0010115;
static constexpr uint32_t MSR_AMD_SMM_CTL       = 0xC0010116;
static constexpr uint32_t MSR_AMD_VM_HSAVE_PA   = 0xC0010117;

// ============================================================================
// AMD VMCB Intercept Bits (Offsets 0x0C, 0x10, 0x14 in VMCB Control Area)
// ============================================================================

// Word 3 (Offset 0x0C): General Intercepts 1
enum SVMIntercept1 : uint32_t {
    SVM_INTERCEPT_INTR              = (1U << 0),
    SVM_INTERCEPT_NMI               = (1U << 1),
    SVM_INTERCEPT_SMI               = (1U << 2),
    SVM_INTERCEPT_INIT              = (1U << 3),
    SVM_INTERCEPT_VINTR             = (1U << 4),
    SVM_INTERCEPT_CR0_SEL_WRITE     = (1U << 5),
    SVM_INTERCEPT_IDTR_READ         = (1U << 6),
    SVM_INTERCEPT_GDTR_READ         = (1U << 7),
    SVM_INTERCEPT_LDTR_READ         = (1U << 8),
    SVM_INTERCEPT_TR_READ           = (1U << 9),
    SVM_INTERCEPT_IDTR_WRITE        = (1U << 10),
    SVM_INTERCEPT_GDTR_WRITE        = (1U << 11),
    SVM_INTERCEPT_LDTR_WRITE        = (1U << 12),
    SVM_INTERCEPT_TR_WRITE          = (1U << 13),
    SVM_INTERCEPT_RDTSC             = (1U << 14),
    SVM_INTERCEPT_RDPMC             = (1U << 15),
    SVM_INTERCEPT_PUSHF             = (1U << 16),
    SVM_INTERCEPT_POPF              = (1U << 17),
    SVM_INTERCEPT_CPUID             = (1U << 18),
    SVM_INTERCEPT_RSM               = (1U << 19),
    SVM_INTERCEPT_IRET              = (1U << 20),
    SVM_INTERCEPT_INTN              = (1U << 21),
    SVM_INTERCEPT_INVD              = (1U << 22),
    SVM_INTERCEPT_PAUSE             = (1U << 23),
    SVM_INTERCEPT_HLT               = (1U << 24),
    SVM_INTERCEPT_INVLPG            = (1U << 25),
    SVM_INTERCEPT_INVLPGA           = (1U << 26),
    SVM_INTERCEPT_IOIO_PROT         = (1U << 27),
    SVM_INTERCEPT_MSR_PROT          = (1U << 28),
    SVM_INTERCEPT_TASK_SWITCH       = (1U << 29),
    SVM_INTERCEPT_FERR_FREEZE       = (1U << 30),
    SVM_INTERCEPT_SHUTDOWN          = (1U << 31),
};

// Word 4 (Offset 0x10): General Intercepts 2
enum SVMIntercept2 : uint32_t {
    SVM_INTERCEPT_VMRUN             = (1U << 0),
    SVM_INTERCEPT_VMMCALL           = (1U << 1),
    SVM_INTERCEPT_VMLOAD            = (1U << 2),
    SVM_INTERCEPT_VMSAVE            = (1U << 3),
    SVM_INTERCEPT_STGI              = (1U << 4),
    SVM_INTERCEPT_CLGI              = (1U << 5),
    SVM_INTERCEPT_SKINIT            = (1U << 6),
    SVM_INTERCEPT_RDTSCP            = (1U << 7),
    SVM_INTERCEPT_ICEBP             = (1U << 8),
    SVM_INTERCEPT_WBINVD            = (1U << 9),
    SVM_INTERCEPT_MONITOR           = (1U << 10),
    SVM_INTERCEPT_MWAIT             = (1U << 11),
    SVM_INTERCEPT_MWAIT_COND        = (1U << 12),
    SVM_INTERCEPT_XSETBV            = (1U << 13),
    SVM_INTERCEPT_RDPRU             = (1U << 14),
    SVM_INTERCEPT_EFER_WRITE_TRAP   = (1U << 15),
    // Bits 16..31 are CR0..CR15 write trap intercepts
};

// Word 5 (Offset 0x14): General Intercepts 3
enum SVMIntercept3 : uint32_t {
    SVM_INTERCEPT_INVLPGB_ALL       = (1U << 0),
    SVM_INTERCEPT_INVLPGB_ILLEGAL   = (1U << 1),
    SVM_INTERCEPT_INVPCID           = (1U << 2),
    SVM_INTERCEPT_MCOMMIT           = (1U << 3),
    SVM_INTERCEPT_TLBSYNC           = (1U << 4),
};

// ============================================================================
// AMD VMCB Exit Codes (Offset 0x70 in VMCB Control Area)
// ============================================================================
enum SVMExitCode : uint64_t {
    SVM_VMEXIT_CR0_READ             = 0x000,
    SVM_VMEXIT_CR3_READ             = 0x003,
    SVM_VMEXIT_CR4_READ             = 0x004,
    SVM_VMEXIT_CR8_READ             = 0x008,
    SVM_VMEXIT_CR0_WRITE            = 0x010,
    SVM_VMEXIT_CR3_WRITE            = 0x013,
    SVM_VMEXIT_CR4_WRITE            = 0x014,
    SVM_VMEXIT_CR8_WRITE            = 0x018,
    SVM_VMEXIT_DR0_READ             = 0x020,
    SVM_VMEXIT_DR15_READ            = 0x02F,
    SVM_VMEXIT_DR0_WRITE            = 0x030,
    SVM_VMEXIT_DR15_WRITE           = 0x03F,
    SVM_VMEXIT_EXCP_BASE            = 0x040,
    SVM_VMEXIT_EXCP_DB              = 0x041,
    SVM_VMEXIT_EXCP_NMI             = 0x042,
    SVM_VMEXIT_EXCP_BP              = 0x043,
    SVM_VMEXIT_EXCP_UD              = 0x046,
    SVM_VMEXIT_EXCP_NM              = 0x047,
    SVM_VMEXIT_EXCP_DF              = 0x048,
    SVM_VMEXIT_EXCP_GP              = 0x04D,
    SVM_VMEXIT_EXCP_PF              = 0x04E,
    SVM_VMEXIT_EXCP_MF              = 0x050,
    SVM_VMEXIT_EXCP_AC              = 0x051,
    SVM_VMEXIT_EXCP_MC              = 0x052,
    SVM_VMEXIT_EXCP_XF              = 0x053,
    SVM_VMEXIT_EXCP_LAST            = 0x05F,
    SVM_VMEXIT_INTR                 = 0x060,
    SVM_VMEXIT_NMI                  = 0x061,
    SVM_VMEXIT_SMI                  = 0x062,
    SVM_VMEXIT_INIT                 = 0x063,
    SVM_VMEXIT_VINTR                = 0x064,
    SVM_VMEXIT_CR0_SEL_WRITE        = 0x065,
    SVM_VMEXIT_IDTR_READ            = 0x066,
    SVM_VMEXIT_GDTR_READ            = 0x067,
    SVM_VMEXIT_LDTR_READ            = 0x068,
    SVM_VMEXIT_TR_READ              = 0x069,
    SVM_VMEXIT_IDTR_WRITE           = 0x06A,
    SVM_VMEXIT_GDTR_WRITE           = 0x06B,
    SVM_VMEXIT_LDTR_WRITE           = 0x06C,
    SVM_VMEXIT_TR_WRITE             = 0x06D,
    SVM_VMEXIT_RDTSC                = 0x06E,
    SVM_VMEXIT_RDPMC                = 0x06F,
    SVM_VMEXIT_PUSHF                = 0x070,
    SVM_VMEXIT_POPF                 = 0x071,
    SVM_VMEXIT_CPUID                = 0x072,
    SVM_VMEXIT_RSM                  = 0x073,
    SVM_VMEXIT_IRET                 = 0x074,
    SVM_VMEXIT_SWINT                = 0x075,
    SVM_VMEXIT_INVD                 = 0x076,
    SVM_VMEXIT_PAUSE                = 0x077,
    SVM_VMEXIT_HLT                  = 0x078,
    SVM_VMEXIT_INVLPG               = 0x079,
    SVM_VMEXIT_INVLPGA              = 0x07A,
    SVM_VMEXIT_IOIO                 = 0x07B,
    SVM_VMEXIT_MSR                  = 0x07C,
    SVM_VMEXIT_TASK_SWITCH          = 0x07D,
    SVM_VMEXIT_FERR_FREEZE          = 0x07E,
    SVM_VMEXIT_SHUTDOWN             = 0x07F,
    SVM_VMEXIT_VMRUN                = 0x080,
    SVM_VMEXIT_VMMCALL              = 0x081,
    SVM_VMEXIT_VMLOAD               = 0x082,
    SVM_VMEXIT_VMSAVE               = 0x083,
    SVM_VMEXIT_STGI                 = 0x084,
    SVM_VMEXIT_CLGI                 = 0x085,
    SVM_VMEXIT_SKINIT               = 0x086,
    SVM_VMEXIT_RDTSCP               = 0x087,
    SVM_VMEXIT_ICEBP                = 0x088,
    SVM_VMEXIT_WBINVD               = 0x089,
    SVM_VMEXIT_MONITOR              = 0x08A,
    SVM_VMEXIT_MWAIT                = 0x08B,
    SVM_VMEXIT_MWAIT_COND           = 0x08C,
    SVM_VMEXIT_XSETBV               = 0x08D,
    SVM_VMEXIT_RDPRU                = 0x08E,
    SVM_VMEXIT_EFER_WRITE_TRAP      = 0x08F,
    SVM_VMEXIT_CR0_WRITE_TRAP       = 0x090,
    SVM_VMEXIT_CR15_WRITE_TRAP      = 0x09F,
    SVM_VMEXIT_INVPCID              = 0x0A2,
    SVM_VMEXIT_NPF                  = 0x400,
    SVM_VMEXIT_AVIC_INCOMPLETE_IPI  = 0x401,
    SVM_VMEXIT_AVIC_NOACCEL         = 0x402,
    SVM_VMEXIT_VMGEXIT              = 0x403,
    SVM_VMEXIT_INVALID              = 0xFFFFFFFFFFFFFFFFULL,
};

// ============================================================================
// AMD VMCB Structures (4096-byte page aligned)
// ============================================================================

struct __attribute__((packed)) SVMSegment {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
};

// VMCB Control Area (0x000 .. 0x3FF, 1024 bytes)
struct __attribute__((packed)) VMCBControlArea {
    uint16_t intercept_cr_read;         // 0x000
    uint16_t intercept_cr_write;        // 0x002
    uint16_t intercept_dr_read;         // 0x004
    uint16_t intercept_dr_write;        // 0x006
    uint32_t intercept_exception;       // 0x008
    uint32_t intercept_ops1;            // 0x00C
    uint32_t intercept_ops2;            // 0x010
    uint32_t intercept_ops3;            // 0x014
    uint8_t  reserved_18[0x3C - 0x18];  // 0x018..0x03B
    uint16_t pause_filter_thresh;       // 0x03C
    uint16_t pause_filter_count;        // 0x03E
    uint64_t iopm_base_pa;              // 0x040
    uint64_t msrpm_base_pa;             // 0x048
    uint64_t tsc_offset;                // 0x050
    uint32_t guest_asid;                // 0x058
    uint8_t  tlb_control;               // 0x05C (0=do nothing, 1=flush all, 3=flush this ASID, 7=flush non-global ASID)
    uint8_t  reserved_5d[3];            // 0x05D..0x05F
    uint64_t vintr;                     // 0x060 (V_TPR[7:0], V_IRQ[8], V_INTR_PRIO[19:16], V_IGN_TPR[20], V_INTR_MASKING[24], V_INTR_VECTOR[39:32])
    uint64_t interrupt_shadow;          // 0x068 (Bit 0: guest in interrupt shadow)
    uint64_t exit_code;                 // 0x070
    uint64_t exit_info_1;               // 0x078
    uint64_t exit_info_2;               // 0x080
    uint64_t exit_int_info;             // 0x088
    uint64_t np_enable;                 // 0x090 (Bit 0: Nested Paging enable)
    uint64_t avic_apic_bar;             // 0x098
    uint64_t ghcb_gpa;                  // 0x0A0
    uint64_t event_inj;                 // 0x0A8
    uint64_t n_cr3;                     // 0x0B0 (Nested Page Table PML4 physical address)
    uint64_t lbr_virtualization_enable; // 0x0B8
    uint32_t vmcb_clean_bits;           // 0x0C0
    uint32_t reserved_c4;               // 0x0C4
    uint64_t next_rip;                  // 0x0C8
    uint8_t  num_bytes_fetched;         // 0x0D0
    uint8_t  guest_inst_bytes[15];      // 0x0D1..0x0DF
    uint64_t avic_apic_backing_page;    // 0x0E0
    uint64_t reserved_e8;               // 0x0E8
    uint64_t avic_logical_table;        // 0x0F0
    uint64_t avic_physical_table;       // 0x0F8
    uint8_t  reserved_100[0x400 - 0x100]; // 0x100..0x3FF
};

static_assert(sizeof(VMCBControlArea) == 0x400, "VMCBControlArea must be 1024 bytes");

// VMCB State Save Area (0x400 .. 0xFFF, 3072 bytes)
struct __attribute__((packed)) VMCBStateSaveArea {
    SVMSegment es;                      // 0x400
    SVMSegment cs;                      // 0x410
    SVMSegment ss;                      // 0x420
    SVMSegment ds;                      // 0x430
    SVMSegment fs;                      // 0x440
    SVMSegment gs;                      // 0x450
    SVMSegment gdtr;                    // 0x460
    SVMSegment ldtr;                    // 0x470
    SVMSegment idtr;                    // 0x480
    SVMSegment tr;                      // 0x490
    uint8_t    reserved_4a0[0x4CB - 0x4A0]; // 0x4A0..0x4CA
    uint8_t    cpl;                     // 0x4CB
    uint32_t   reserved_4cc;            // 0x4CC
    uint64_t   efer;                    // 0x4D0
    uint8_t    reserved_4d8[0x548 - 0x4D8]; // 0x4D8..0x547
    uint64_t   cr4;                     // 0x548
    uint64_t   cr3;                     // 0x550
    uint64_t   cr0;                     // 0x558
    uint64_t   dr7;                     // 0x560
    uint64_t   dr6;                     // 0x568
    uint64_t   rflags;                  // 0x570
    uint64_t   rip;                     // 0x578
    uint8_t    reserved_580[0x5D8 - 0x580]; // 0x580..0x5D7
    uint64_t   rsp;                     // 0x5D8
    uint8_t    reserved_5e0[0x5F8 - 0x5E0]; // 0x5E0..0x5F7
    uint64_t   rax;                     // 0x5F8
    uint64_t   star;                    // 0x600
    uint64_t   lstar;                   // 0x608
    uint64_t   cstar;                   // 0x610
    uint64_t   sfmask;                  // 0x618
    uint64_t   kernel_gs_base;          // 0x620
    uint64_t   sysenter_cs;             // 0x628
    uint64_t   sysenter_esp;            // 0x630
    uint64_t   sysenter_eip;            // 0x638
    uint64_t   cr2;                     // 0x640
    uint8_t    reserved_648[0x668 - 0x648]; // 0x648..0x667
    uint64_t   g_pat;                   // 0x668
    uint64_t   dbgctl;                  // 0x670
    uint64_t   br_from;                 // 0x678
    uint64_t   br_to;                   // 0x680
    uint64_t   last_excp_from;          // 0x688
    uint64_t   last_excp_to;            // 0x690
    uint8_t    reserved_698[0x1000 - 0x698]; // 0x698..0xFFF
};

static_assert(sizeof(VMCBStateSaveArea) == 0xC00, "VMCBStateSaveArea must be 3072 bytes");

struct __attribute__((packed, aligned(4096))) VMCB {
    VMCBControlArea   control;
    VMCBStateSaveArea state;
};

static_assert(sizeof(VMCB) == 0x1000, "VMCB must be 4096 bytes");

// ============================================================================
// AMD VMCB VINTR Bits (Offset 0x060)
// ============================================================================
static constexpr uint64_t SVM_VINTR_TPR_MASK        = 0xFFULL;
static constexpr uint64_t SVM_VINTR_IRQ             = (1ULL << 8);
static constexpr uint64_t SVM_VINTR_VGIF            = (1ULL << 9);
static constexpr uint64_t SVM_VINTR_PRIO_SHIFT      = 16;
static constexpr uint64_t SVM_VINTR_PRIO_MASK       = (0xFULL << 16);
static constexpr uint64_t SVM_VINTR_IGN_TPR         = (1ULL << 20);
static constexpr uint64_t SVM_VINTR_INTR_MASKING    = (1ULL << 24);
static constexpr uint64_t SVM_VINTR_AVIC_ENABLE     = (1ULL << 31);
static constexpr uint64_t SVM_VINTR_VECTOR_SHIFT    = 32;
static constexpr uint64_t SVM_VINTR_VECTOR_MASK     = (0xFFULL << 32);

// ============================================================================
// AMD SVM #NPF EXITINFO1 Bits
// ============================================================================
static constexpr uint64_t SVM_NPF_INFO1_PRESENT     = (1ULL << 0);
static constexpr uint64_t SVM_NPF_INFO1_WRITE       = (1ULL << 1);
static constexpr uint64_t SVM_NPF_INFO1_USER        = (1ULL << 2);
static constexpr uint64_t SVM_NPF_INFO1_RSVD        = (1ULL << 3);
static constexpr uint64_t SVM_NPF_INFO1_FETCH       = (1ULL << 4);
static constexpr uint64_t SVM_NPF_INFO1_FINAL_GPA   = (1ULL << 32);
static constexpr uint64_t SVM_NPF_INFO1_GPT_ACCESS  = (1ULL << 33);

#endif // AMDHV_AMD_SVM_HPP
