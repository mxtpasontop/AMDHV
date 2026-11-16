#ifndef AMDHV_INTEL_VMX_HPP
#define AMDHV_INTEL_VMX_HPP

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Intel Architecture & VMX MSRs
// ============================================================================
enum IntelMSR : uint32_t {
    MSR_IA32_TSC                    = 0x00000010,
    MSR_IA32_APIC_BASE              = 0x0000001B,
    MSR_IA32_FEATURE_CONTROL        = 0x0000003A,
    MSR_IA32_TSC_ADJUST             = 0x0000003B,
    MSR_IA32_SPEC_CTRL              = 0x00000048,
    MSR_IA32_PRED_CMD               = 0x00000049,
    MSR_IA32_BIOS_SIGN_ID           = 0x0000008B,
    MSR_IA32_MTRRCAP                = 0x000000FE,
    MSR_IA32_ARCH_CAPABILITIES      = 0x0000010A,
    MSR_IA32_FLUSH_CMD              = 0x0000010B,
    MSR_IA32_SYSENTER_CS            = 0x00000174,
    MSR_IA32_SYSENTER_ESP           = 0x00000175,
    MSR_IA32_SYSENTER_EIP           = 0x00000176,
    MSR_IA32_MCG_CAP                = 0x00000179,
    MSR_IA32_MCG_STATUS             = 0x0000017A,
    MSR_IA32_MISC_ENABLE            = 0x000001A0,
    MSR_IA32_DEBUGCTL               = 0x000001D9,
    MSR_IA32_PAT                    = 0x00000277,
    MSR_IA32_MTRR_DEF_TYPE          = 0x000002FF,
    MSR_IA32_PERF_CAPABILITIES      = 0x00000345,
    MSR_IA32_FIXED_CTR_CTRL         = 0x0000038D,
    MSR_IA32_PERF_GLOBAL_STATUS     = 0x0000038E,
    MSR_IA32_PERF_GLOBAL_CTRL       = 0x0000038F,
    MSR_IA32_PERF_GLOBAL_OVF_CTRL   = 0x00000390,

    // Intel VMX Capability MSRs (0x480 .. 0x491)
    MSR_IA32_VMX_BASIC              = 0x00000480,
    MSR_IA32_VMX_PINBASED_CTLS      = 0x00000481,
    MSR_IA32_VMX_PROCBASED_CTLS     = 0x00000482,
    MSR_IA32_VMX_EXIT_CTLS          = 0x00000483,
    MSR_IA32_VMX_ENTRY_CTLS         = 0x00000484,
    MSR_IA32_VMX_MISC               = 0x00000485,
    MSR_IA32_VMX_CR0_FIXED0         = 0x00000486,
    MSR_IA32_VMX_CR0_FIXED1         = 0x00000487,
    MSR_IA32_VMX_CR4_FIXED0         = 0x00000488,
    MSR_IA32_VMX_CR4_FIXED1         = 0x00000489,
    MSR_IA32_VMX_VMCS_ENUM          = 0x0000048A,
    MSR_IA32_VMX_PROCBASED_CTLS2    = 0x0000048B,
    MSR_IA32_VMX_EPT_VPID_CAP       = 0x0000048C,
    MSR_IA32_VMX_TRUE_PINBASED_CTLS = 0x0000048D,
    MSR_IA32_VMX_TRUE_PROCBASED_CTLS= 0x0000048E,
    MSR_IA32_VMX_TRUE_EXIT_CTLS     = 0x0000048F,
    MSR_IA32_VMX_TRUE_ENTRY_CTLS    = 0x00000490,
    MSR_IA32_VMX_VMFUNC             = 0x00000491,

    // x86-64 Architectural MSRs
    MSR_IA32_EFER                   = 0xC0000080,
    MSR_IA32_STAR                   = 0xC0000081,
    MSR_IA32_LSTAR                  = 0xC0000082,
    MSR_IA32_CSTAR                  = 0xC0000083,
    MSR_IA32_FMASK                  = 0xC0000084,
    MSR_IA32_FS_BASE                = 0xC0000100,
    MSR_IA32_GS_BASE                = 0xC0000101,
    MSR_IA32_KERNEL_GS_BASE         = 0xC0000102,
    MSR_IA32_TSC_AUX                = 0xC0000103,
};

// ============================================================================
// Intel VMCS Field Encodings ( matching AppleHV.kext )
// ============================================================================
enum VMCSField : uint32_t {
    // 16-bit Control Fields
    VMCS_VPID                           = 0x0000,
    VMCS_CTRL_POSTED_INT_NOTIFY_VECTOR  = 0x0002,
    VMCS_CTRL_EPTP_INDEX                = 0x0004,

    // 16-bit Guest-State Fields
    VMCS_GUEST_ES                       = 0x0800,
    VMCS_GUEST_CS                       = 0x0802,
    VMCS_GUEST_SS                       = 0x0804,
    VMCS_GUEST_DS                       = 0x0806,
    VMCS_GUEST_FS                       = 0x0808,
    VMCS_GUEST_GS                       = 0x080A,
    VMCS_GUEST_LDTR                     = 0x080C,
    VMCS_GUEST_TR                       = 0x080E,
    VMCS_GUEST_INT_STATUS               = 0x0810,
    VMCS_GUEST_PML_INDEX                = 0x0812,

    // 16-bit Host-State Fields
    VMCS_HOST_ES                        = 0x0C00,
    VMCS_HOST_CS                        = 0x0C02,
    VMCS_HOST_SS                        = 0x0C04,
    VMCS_HOST_DS                        = 0x0C06,
    VMCS_HOST_FS                        = 0x0C08,
    VMCS_HOST_GS                        = 0x0C0A,
    VMCS_HOST_TR                        = 0x0C0C,

    // 64-bit Control Fields
    VMCS_CTRL_IO_BITMAP_A               = 0x2000,
    VMCS_CTRL_IO_BITMAP_B               = 0x2002,
    VMCS_CTRL_MSR_BITMAPS               = 0x2004,
    VMCS_CTRL_VMEXIT_MSR_STORE_ADDR     = 0x2006,
    VMCS_CTRL_VMEXIT_MSR_LOAD_ADDR      = 0x2008,
    VMCS_CTRL_VMENTRY_MSR_LOAD_ADDR     = 0x200A,
    VMCS_CTRL_EXECUTIVE_VMCS_PTR        = 0x200C,
    VMCS_CTRL_PML_ADDR                  = 0x200E,
    VMCS_CTRL_TSC_OFFSET                = 0x2010,
    VMCS_CTRL_VIRTUAL_APIC              = 0x2012,
    VMCS_CTRL_APIC_ACCESS               = 0x2014,
    VMCS_CTRL_POSTED_INT_DESC_ADDR      = 0x2016,
    VMCS_CTRL_VMFUNC_CONTROLS           = 0x2018,
    VMCS_CTRL_EPTP                      = 0x201A,
    VMCS_CTRL_EOI_EXIT_BITMAP_0         = 0x201C,
    VMCS_CTRL_EOI_EXIT_BITMAP_1         = 0x201E,
    VMCS_CTRL_EOI_EXIT_BITMAP_2         = 0x2020,
    VMCS_CTRL_EOI_EXIT_BITMAP_3         = 0x2022,
    VMCS_CTRL_EPTP_LIST_ADDR            = 0x2024,
    VMCS_CTRL_VMREAD_BITMAP_ADDR        = 0x2026,
    VMCS_CTRL_VMWRITE_BITMAP_ADDR       = 0x2028,
    VMCS_CTRL_VIRT_EXC_INFO_ADDR        = 0x202A,
    VMCS_CTRL_XSS_EXITING_BITMAP        = 0x202C,
    VMCS_CTRL_ENCLS_EXITING_BITMAP      = 0x202E,
    VMCS_CTRL_TSC_MULTIPLIER            = 0x2032,

    // 64-bit Read-Only Data Field
    VMCS_GUEST_PHYSICAL_ADDRESS         = 0x2400,

    // 64-bit Guest-State Fields
    VMCS_GUEST_LINK_POINTER             = 0x2800,
    VMCS_GUEST_IA32_DEBUGCTL            = 0x2802,
    VMCS_GUEST_IA32_PAT                 = 0x2804,
    VMCS_GUEST_IA32_EFER                = 0x2806,
    VMCS_GUEST_IA32_PERF_GLOBAL_CTRL    = 0x2808,
    VMCS_GUEST_PDPTE0                   = 0x280A,
    VMCS_GUEST_PDPTE1                   = 0x280C,
    VMCS_GUEST_PDPTE2                   = 0x280E,
    VMCS_GUEST_PDPTE3                   = 0x2810,
    VMCS_GUEST_IA32_BNDCFGS             = 0x2812,

    // 64-bit Host-State Fields
    VMCS_HOST_IA32_PAT                  = 0x2C00,
    VMCS_HOST_IA32_EFER                 = 0x2C02,
    VMCS_HOST_IA32_PERF_GLOBAL_CTRL     = 0x2C04,

    // 32-bit Control Fields
    VMCS_CTRL_PIN_BASED                 = 0x4000,
    VMCS_CTRL_CPU_BASED                 = 0x4002,
    VMCS_CTRL_EXC_BITMAP                = 0x4004,
    VMCS_CTRL_PF_ERROR_MASK             = 0x4006,
    VMCS_CTRL_PF_ERROR_MATCH            = 0x4008,
    VMCS_CTRL_CR3_COUNT                 = 0x400A,
    VMCS_CTRL_VMEXIT_CONTROLS           = 0x400C,
    VMCS_CTRL_VMEXIT_MSR_STORE_COUNT    = 0x400E,
    VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT     = 0x4010,
    VMCS_CTRL_VMENTRY_CONTROLS          = 0x4012,
    VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT    = 0x4014,
    VMCS_CTRL_VMENTRY_IRQ_INFO          = 0x4016,
    VMCS_CTRL_VMENTRY_EXC_ERROR         = 0x4018,
    VMCS_CTRL_VMENTRY_INSTR_LEN         = 0x401A,
    VMCS_CTRL_TPR_THRESHOLD             = 0x401C,
    VMCS_CTRL_CPU_BASED2                = 0x401E,
    VMCS_CTRL_PLE_GAP                   = 0x4020,
    VMCS_CTRL_PLE_WINDOW                = 0x4022,

    // 32-bit Read-Only Data Fields
    VMCS_RO_INSTR_ERROR                 = 0x4400,
    VMCS_RO_EXIT_REASON                 = 0x4402,
    VMCS_RO_VMEXIT_IRQ_INFO             = 0x4404,
    VMCS_RO_VMEXIT_IRQ_ERROR            = 0x4406,
    VMCS_RO_IDT_VECTOR_INFO             = 0x4408,
    VMCS_RO_IDT_VECTOR_ERROR            = 0x440A,
    VMCS_RO_VMEXIT_INSTR_LEN            = 0x440C,
    VMCS_RO_VMEXIT_INSTR_INFO           = 0x440E,

    // 32-bit Guest-State Fields
    VMCS_GUEST_ES_LIMIT                 = 0x4800,
    VMCS_GUEST_CS_LIMIT                 = 0x4802,
    VMCS_GUEST_SS_LIMIT                 = 0x4804,
    VMCS_GUEST_DS_LIMIT                 = 0x4806,
    VMCS_GUEST_FS_LIMIT                 = 0x4808,
    VMCS_GUEST_GS_LIMIT                 = 0x480A,
    VMCS_GUEST_LDTR_LIMIT               = 0x480C,
    VMCS_GUEST_TR_LIMIT                 = 0x480E,
    VMCS_GUEST_GDTR_LIMIT               = 0x4810,
    VMCS_GUEST_IDTR_LIMIT               = 0x4812,
    VMCS_GUEST_ES_AR                    = 0x4814,
    VMCS_GUEST_CS_AR                    = 0x4816,
    VMCS_GUEST_SS_AR                    = 0x4818,
    VMCS_GUEST_DS_AR                    = 0x481A,
    VMCS_GUEST_FS_AR                    = 0x481C,
    VMCS_GUEST_GS_AR                    = 0x481E,
    VMCS_GUEST_LDTR_AR                  = 0x4820,
    VMCS_GUEST_TR_AR                    = 0x4822,
    VMCS_GUEST_IGNORE_IRQ               = 0x4824,
    VMCS_GUEST_ACTIVITY_STATE           = 0x4826,
    VMCS_GUEST_SMBASE                   = 0x4828,
    VMCS_GUEST_IA32_SYSENTER_CS         = 0x482A,
    VMCS_GUEST_VMX_TIMER_VALUE          = 0x482E,

    // 32-bit Host-State Field
    VMCS_HOST_IA32_SYSENTER_CS          = 0x4C00,

    // Natural-Width (64-bit) Control Fields
    VMCS_CTRL_CR0_MASK                  = 0x6000,
    VMCS_CTRL_CR4_MASK                  = 0x6002,
    VMCS_CTRL_CR0_SHADOW                = 0x6004,
    VMCS_CTRL_CR4_SHADOW                = 0x6006,
    VMCS_CTRL_CR3_VALUE0                = 0x6008,
    VMCS_CTRL_CR3_VALUE1                = 0x600A,
    VMCS_CTRL_CR3_VALUE2                = 0x600C,
    VMCS_CTRL_CR3_VALUE3                = 0x600E,

    // Natural-Width (64-bit) Read-Only Data Fields
    VMCS_RO_EXIT_QUALIFIC               = 0x6400,
    VMCS_RO_IO_RCX                      = 0x6402,
    VMCS_RO_IO_RSI                      = 0x6404,
    VMCS_RO_IO_RDI                      = 0x6406,
    VMCS_RO_IO_RIP                      = 0x6408,
    VMCS_RO_GUEST_LIN_ADDR              = 0x640A,

    // Natural-Width (64-bit) Guest-State Fields
    VMCS_GUEST_CR0                      = 0x6800,
    VMCS_GUEST_CR3                      = 0x6802,
    VMCS_GUEST_CR4                      = 0x6804,
    VMCS_GUEST_ES_BASE                  = 0x6806,
    VMCS_GUEST_CS_BASE                  = 0x6808,
    VMCS_GUEST_SS_BASE                  = 0x680A,
    VMCS_GUEST_DS_BASE                  = 0x680C,
    VMCS_GUEST_FS_BASE                  = 0x680E,
    VMCS_GUEST_GS_BASE                  = 0x6810,
    VMCS_GUEST_LDTR_BASE                = 0x6812,
    VMCS_GUEST_TR_BASE                  = 0x6814,
    VMCS_GUEST_GDTR_BASE                = 0x6816,
    VMCS_GUEST_IDTR_BASE                = 0x6818,
    VMCS_GUEST_DR7                      = 0x681A,
    VMCS_GUEST_RSP                      = 0x681C,
    VMCS_GUEST_RIP                      = 0x681E,
    VMCS_GUEST_RFLAGS                   = 0x6820,
    VMCS_GUEST_DEBUG_EXC                = 0x6822,
    VMCS_GUEST_SYSENTER_ESP             = 0x6824,
    VMCS_GUEST_SYSENTER_EIP             = 0x6826,

    // Natural-Width (64-bit) Host-State Fields
    VMCS_HOST_CR0                       = 0x6C00,
    VMCS_HOST_CR3                       = 0x6C02,
    VMCS_HOST_CR4                       = 0x6C04,
    VMCS_HOST_FS_BASE                   = 0x6C06,
    VMCS_HOST_GS_BASE                   = 0x6C08,
    VMCS_HOST_TR_BASE                   = 0x6C0A,
    VMCS_HOST_GDTR_BASE                 = 0x6C0C,
    VMCS_HOST_IDTR_BASE                 = 0x6C0E,
    VMCS_HOST_IA32_SYSENTER_ESP         = 0x6C10,
    VMCS_HOST_IA32_SYSENTER_EIP         = 0x6C12,
    VMCS_HOST_RSP                       = 0x6C14,
    VMCS_HOST_RIP                       = 0x6C16,
};

// ============================================================================
// Intel VMX Control Bits
// ============================================================================
enum VMXPinBasedControls : uint32_t {
    PIN_BASED_INTR_EXIT                 = (1U << 0),
    PIN_BASED_NMI_EXIT                  = (1U << 3),
    PIN_BASED_VIRTUAL_NMIS              = (1U << 5),
    PIN_BASED_PREEMPTION_TIMER          = (1U << 6),
    PIN_BASED_POSTED_INTERRUPTS         = (1U << 7),
};

enum VMXProcBasedControls : uint32_t {
    CPU_BASED_IRQ_WND_EXIT              = (1U << 2),
    CPU_BASED_USE_TSC_OFFSET            = (1U << 3),
    CPU_BASED_HLT_EXIT                  = (1U << 7),
    CPU_BASED_INVLPG_EXIT               = (1U << 9),
    CPU_BASED_MWAIT_EXIT                = (1U << 10),
    CPU_BASED_RDPMC_EXIT                = (1U << 11),
    CPU_BASED_RDTSC_EXIT                = (1U << 12),
    CPU_BASED_CR3_LOAD_EXIT             = (1U << 15),
    CPU_BASED_CR3_STORE_EXIT            = (1U << 16),
    CPU_BASED_CR8_LOAD_EXIT             = (1U << 19),
    CPU_BASED_CR8_STORE_EXIT            = (1U << 20),
    CPU_BASED_TPR_SHADOW                = (1U << 21),
    CPU_BASED_NMI_WND_EXIT              = (1U << 22),
    CPU_BASED_MOV_DR_EXIT               = (1U << 23),
    CPU_BASED_UNCOND_IO_EXIT            = (1U << 24),
    CPU_BASED_USE_IO_BITMAPS            = (1U << 25),
    CPU_BASED_MONITOR_TRAP_FLAG         = (1U << 27),
    CPU_BASED_USE_MSR_BITMAPS           = (1U << 28),
    CPU_BASED_MONITOR_EXIT              = (1U << 29),
    CPU_BASED_PAUSE_EXIT                = (1U << 30),
    CPU_BASED_ACTIVATE_SEC_CTLS         = (1U << 31),
};

enum VMXProcBasedControls2 : uint32_t {
    CPU_BASED2_VIRT_APIC_ACCESSES       = (1U << 0),
    CPU_BASED2_ENABLE_EPT               = (1U << 1),
    CPU_BASED2_DESC_TABLE_EXIT          = (1U << 2),
    CPU_BASED2_ENABLE_RDTSCP            = (1U << 3),
    CPU_BASED2_VIRTUALIZE_X2APIC        = (1U << 4),
    CPU_BASED2_ENABLE_VPID              = (1U << 5),
    CPU_BASED2_WBINVD_EXIT              = (1U << 6),
    CPU_BASED2_UNRESTRICTED_GUEST       = (1U << 7),
    CPU_BASED2_APIC_REG_VIRT            = (1U << 8),
    CPU_BASED2_VIRT_INTR_DELIVERY       = (1U << 9),
    CPU_BASED2_PAUSE_LOOP_EXIT          = (1U << 10),
    CPU_BASED2_RDRAND_EXIT              = (1U << 11),
    CPU_BASED2_ENABLE_INVPCID           = (1U << 12),
    CPU_BASED2_ENABLE_VMFUNC            = (1U << 13),
    CPU_BASED2_VMCS_SHADOWING           = (1U << 14),
    CPU_BASED2_ENABLE_ENCLS_EXIT        = (1U << 15),
    CPU_BASED2_RDSEED_EXIT              = (1U << 16),
    CPU_BASED2_ENABLE_PML               = (1U << 17),
    CPU_BASED2_EPT_VE                   = (1U << 18),
    CPU_BASED2_ENABLE_XSAVES            = (1U << 20),
    CPU_BASED2_MODE_BASED_EPT_EXEC      = (1U << 22),
    CPU_BASED2_USE_TSC_SCALING          = (1U << 25),
};

enum VMXExitControls : uint32_t {
    VMEXIT_SAVE_DEBUG_CONTROLS          = (1U << 2),
    VMEXIT_HOST_ADDR_SPACE_SIZE         = (1U << 9),
    VMEXIT_LOAD_IA32_PERF_GLOBAL_CTRL   = (1U << 12),
    VMEXIT_ACK_INTR_ON_EXIT             = (1U << 15),
    VMEXIT_SAVE_IA32_PAT                = (1U << 18),
    VMEXIT_LOAD_IA32_PAT                = (1U << 19),
    VMEXIT_SAVE_IA32_EFER               = (1U << 20),
    VMEXIT_LOAD_IA32_EFER               = (1U << 21),
    VMEXIT_SAVE_VMX_TIMER_VALUE         = (1U << 22),
};

enum VMXEntryControls : uint32_t {
    VMENTRY_LOAD_DEBUG_CONTROLS         = (1U << 2),
    VMENTRY_IA32E_MODE_GUEST            = (1U << 9),
    VMENTRY_ENTRY_TO_SMM                = (1U << 10),
    VMENTRY_DEACTIVATE_DUAL_MONITOR     = (1U << 11),
    VMENTRY_LOAD_IA32_PERF_GLOBAL_CTRL  = (1U << 13),
    VMENTRY_LOAD_IA32_PAT               = (1U << 14),
    VMENTRY_LOAD_IA32_EFER              = (1U << 15),
};

// ============================================================================
// Intel VMX Exit Reasons ( Basic Exit Reasons 0..68 )
// ============================================================================
enum VMXExitReason : uint32_t {
    VMX_REASON_EXC_NMI                  = 0,
    VMX_REASON_EXT_IRQ                  = 1,
    VMX_REASON_TRIPLE_FAULT             = 2,
    VMX_REASON_INIT                     = 3,
    VMX_REASON_SIPI                     = 4,
    VMX_REASON_IO_SMI                   = 5,
    VMX_REASON_OTHER_SMI                = 6,
    VMX_REASON_IRQ_WND                  = 7,
    VMX_REASON_NMI_WND                  = 8,
    VMX_REASON_TASK_SWITCH              = 9,
    VMX_REASON_CPUID                    = 10,
    VMX_REASON_GETSEC                   = 11,
    VMX_REASON_HLT                      = 12,
    VMX_REASON_INVD                     = 13,
    VMX_REASON_INVLPG                   = 14,
    VMX_REASON_RDPMC                    = 15,
    VMX_REASON_RDTSC                    = 16,
    VMX_REASON_RSM                      = 17,
    VMX_REASON_VMCALL                   = 18,
    VMX_REASON_VMCLEAR                  = 19,
    VMX_REASON_VMLAUNCH                 = 20,
    VMX_REASON_VMPTRLD                  = 21,
    VMX_REASON_VMPTRST                  = 22,
    VMX_REASON_VMREAD                   = 23,
    VMX_REASON_VMRESUME                 = 24,
    VMX_REASON_VMWRITE                  = 25,
    VMX_REASON_VMXOFF                   = 26,
    VMX_REASON_VMXON                    = 27,
    VMX_REASON_MOV_CR                   = 28,
    VMX_REASON_MOV_DR                   = 29,
    VMX_REASON_IO                       = 30,
    VMX_REASON_RDMSR                    = 31,
    VMX_REASON_WRMSR                    = 32,
    VMX_REASON_VMENTRY_GUEST            = 33,
    VMX_REASON_VMENTRY_MSR              = 34,
    VMX_REASON_MWAIT                    = 36,
    VMX_REASON_MTF                      = 37,
    VMX_REASON_MONITOR                  = 39,
    VMX_REASON_PAUSE                    = 40,
    VMX_REASON_VMENTRY_MC               = 41,
    VMX_REASON_TPR_THRESHOLD            = 43,
    VMX_REASON_APIC_ACCESS              = 44,
    VMX_REASON_VIRTUALIZED_EOI          = 45,
    VMX_REASON_GDTR_IDTR                = 46,
    VMX_REASON_LDTR_TR                  = 47,
    VMX_REASON_EPT_VIOLATION            = 48,
    VMX_REASON_EPT_MISCONFIG            = 49,
    VMX_REASON_INVEPT                   = 50,
    VMX_REASON_RDTSCP                   = 51,
    VMX_REASON_VMX_TIMER_EXPIRED        = 52,
    VMX_REASON_INVVPID                  = 53,
    VMX_REASON_WBINVD                   = 54,
    VMX_REASON_XSETBV                   = 55,
    VMX_REASON_APIC_WRITE               = 56,
    VMX_REASON_RDRAND                   = 57,
    VMX_REASON_INVPCID                  = 58,
    VMX_REASON_VMFUNC                   = 59,
    VMX_REASON_ENCLS                    = 60,
    VMX_REASON_RDSEED                   = 61,
    VMX_REASON_PML_FULL                 = 62,
    VMX_REASON_XSAVES                   = 63,
    VMX_REASON_XRSTORS                  = 64,
};

// ============================================================================
// AppleHV.kext Internal Structures (Reversed from AppleHV binary)
// ============================================================================

struct hv_vmx_msr_entry_t {
    uint32_t index;
    uint32_t reserved;
    uint64_t data;
};

// GPR array passed in %rsi to _hv_vmx_vcpu_resume_or_launch_guest(is_launched, gprs)
// Note: %rsi points to offset 0xC80 of hv_vmx_vcpu_rw_interface_t:
//   0x00(%rsi): RIP
//   0x08(%rsi): RFLAGS
//   0x10(%rsi): RAX
//   0x18(%rsi): RCX
//   0x20(%rsi): RDX
//   0x28(%rsi): RBX
//   0x30(%rsi): RSI
//   0x38(%rsi): RDI
//   0x40(%rsi): RSP
//   0x48(%rsi): RBP
//   0x50(%rsi): R8
//   0x58(%rsi): R9
//   0x60(%rsi): R10
//   0x68(%rsi): R11
//   0x70(%rsi): R12
//   0x78(%rsi): R13
//   0x80(%rsi): R14
//   0x88(%rsi): R15
//   0x130(%rsi): CR2
struct AppleHVGPRFrame {
    uint64_t rip;       // 0x00
    uint64_t rflags;    // 0x08
    uint64_t rax;       // 0x10
    uint64_t rcx;       // 0x18
    uint64_t rdx;       // 0x20
    uint64_t rbx;       // 0x28
    uint64_t rsi;       // 0x30
    uint64_t rdi;       // 0x38
    uint64_t rsp;       // 0x40
    uint64_t rbp;       // 0x48
    uint64_t r8;        // 0x50
    uint64_t r9;        // 0x58
    uint64_t r10;       // 0x60
    uint64_t r11;       // 0x68
    uint64_t r12;       // 0x70
    uint64_t r13;       // 0x78
    uint64_t r14;       // 0x80
    uint64_t r15;       // 0x88
    uint8_t  pad[0xA0]; // 0x90..0x12F
    uint64_t cr2;       // 0x130
};

// Reversed layout of hv_vmx_space_t (size 0x40)
struct AppleHVSpace {
    void     *vtable;               // 0x00
    uint64_t  objectReferenceState;                 // 0x08 OSObject reference state; not a lock
    void     *vm_map;               // 0x10
    uint64_t  eptRootAndFlags;                 // 0x18 (XNU EPTP/root; get_eptp normalizes flags)
    void     *pmap;                 // 0x20 (XNU pmap_t, where *(uint64_t*)((uint8_t*)pmap + 0x50) is pm_pml4 VA)
    bool      ad_supported;         // 0x28
    bool      exec_only_supported;  // 0x29
    bool      mbe_supported;        // 0x2A
    uint8_t   pad[5];               // 0x2B..0x2F
    void     *task;                 // 0x30
    uint32_t  refcount;             // 0x38
    uint32_t  pad2;                 // 0x3C
};

// Key offsets in hv_vmx_vcpu_t (reversed from AppleHV.kext)
static constexpr size_t APPLEHV_VCPU_OFFSET_ID              = 0x24;
static constexpr size_t APPLEHV_VCPU_OFFSET_VM              = 0x28;
static constexpr size_t APPLEHV_VCPU_OFFSET_SPACE           = 0x30;
static constexpr size_t APPLEHV_VCPU_OFFSET_RO_IF           = 0x40;
static constexpr size_t APPLEHV_VCPU_OFFSET_RW_IF           = 0x38;
static constexpr size_t APPLEHV_VCPU_OFFSET_VMCS_PA         = 0x90;
static constexpr size_t APPLEHV_VCPU_OFFSET_IS_LAUNCHED     = 0x385;

// ============================================================================
// XNU x86_saved_state_t layout (passed to _kernel_trap in %rdi)
// ============================================================================
struct XNUSavedState64 {
    uint32_t flavor;        // 0x00 (0xF = x86_SAVED_STATE64)
    uint32_t pad0;          // 0x04
    uint64_t pad1;          // 0x08
    uint64_t rdi;           // 0x10
    uint64_t rsi;           // 0x18
    uint64_t rdx;           // 0x20
    uint64_t r10;           // 0x28
    uint64_t r8;            // 0x30
    uint64_t r9;            // 0x38
    uint64_t cr2;           // 0x40
    uint64_t r15;           // 0x48
    uint64_t r14;           // 0x50
    uint64_t r13;           // 0x58
    uint64_t r12;           // 0x60
    uint64_t r11;           // 0x68
    uint64_t rbp;           // 0x70
    uint64_t rbx;           // 0x78
    uint64_t rcx;           // 0x80
    uint64_t rax;           // 0x88
    uint32_t gs;            // 0x90
    uint32_t fs;            // 0x94
    uint64_t pad2;          // 0x98
    uint16_t trapno;        // 0xA0 (6 = #UD, 13 = #GP, 14 = #PF)
    uint16_t cpu;           // 0xA2
    uint32_t pad3;          // 0xA4
    uint64_t trapfn;        // 0xA8
    uint64_t err;           // 0xB0
    uint64_t rip;           // 0xB8
    uint64_t cs;            // 0xC0
    uint64_t rflags;        // 0xC8
    uint64_t rsp;           // 0xD0
    uint64_t ss;            // 0xD8
};

static_assert(sizeof(AppleHVGPRFrame)==0x138,"AppleHV frame extent");
static_assert(offsetof(AppleHVGPRFrame,rax)==0x10 && offsetof(AppleHVGPRFrame,r15)==0x88 && offsetof(AppleHVGPRFrame,cr2)==0x130,"AppleHV GPR offsets");
static_assert(sizeof(AppleHVSpace)==0x40 && offsetof(AppleHVSpace,eptRootAndFlags)==0x18,"AppleHV space offsets");
static_assert(sizeof(XNUSavedState64)==0xe0 && offsetof(XNUSavedState64,rip)==0xb8 && offsetof(XNUSavedState64,cpu)==0xa2,"XNU trap offsets");

static constexpr uint16_t XNU_TRAP_INVALID_OPCODE      = 6;
static constexpr uint16_t XNU_TRAP_GENERAL_PROTECTION  = 13;
static constexpr uint16_t XNU_TRAP_PAGE_FAULT          = 14;

// RFLAGS bits used by VMX instructions
static constexpr uint64_t RFLAGS_CF = (1ULL << 0);
static constexpr uint64_t RFLAGS_PF = (1ULL << 2);
static constexpr uint64_t RFLAGS_AF = (1ULL << 4);
static constexpr uint64_t RFLAGS_ZF = (1ULL << 6);
static constexpr uint64_t RFLAGS_SF = (1ULL << 7);
static constexpr uint64_t RFLAGS_OF = (1ULL << 11);
static constexpr uint64_t RFLAGS_VMX_MASK = (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF);

#endif // AMDHV_INTEL_VMX_HPP
