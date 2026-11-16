#ifndef AMDHV_ARCHITECTURE_HPP
#define AMDHV_ARCHITECTURE_HPP
#include <src/IntelVMX.hpp>
// Offline translation contract. This is deliberately smaller than AppleHV's
// requirements. Unsupported controls are rejected, never advertised to placate it.
namespace Architecture {
constexpr uint32_t pin = PIN_BASED_INTR_EXIT | PIN_BASED_NMI_EXIT;
constexpr uint32_t primary = CPU_BASED_USE_TSC_OFFSET | CPU_BASED_HLT_EXIT |
    CPU_BASED_RDPMC_EXIT | CPU_BASED_RDTSC_EXIT | CPU_BASED_UNCOND_IO_EXIT |
    CPU_BASED_ACTIVATE_SEC_CTLS | CPU_BASED_PAUSE_EXIT;
constexpr uint32_t secondary = CPU_BASED2_ENABLE_EPT | CPU_BASED2_UNRESTRICTED_GUEST |
    CPU_BASED2_WBINVD_EXIT;
constexpr uint32_t entry = VMENTRY_IA32E_MODE_GUEST | VMENTRY_LOAD_IA32_PAT | VMENTRY_LOAD_IA32_EFER;
constexpr uint32_t exit = VMEXIT_HOST_ADDR_SPACE_SIZE | VMEXIT_SAVE_IA32_PAT |
    VMEXIT_LOAD_IA32_PAT | VMEXIT_SAVE_IA32_EFER | VMEXIT_LOAD_IA32_EFER;
constexpr uint64_t capability(uint32_t allowed) { return uint64_t(allowed)<<32; }
inline bool knownException(uint32_t vector) {
    switch(vector) {
        case 0:case 1:case 3:case 4:case 5:case 6:case 7:case 8:
        case 10:case 11:case 12:case 13:case 14:case 16:case 17:
        case 18:case 19:case 20:case 21:return true;
        default:return false;
    }
}
// Only event formats whose SVM and VMX semantics agree without RIP rewriting.
inline bool toSVMEvent(uint32_t info,uint32_t error,uint64_t &event) {
    event=0;
    if (!(info&(1U<<31))) return true;
    if (info&~0x80000fffU) return false;
    uint32_t vector=info&255,type=(info>>8)&7;
    if (type!=0 && type!=2 && type!=3) return false;
    if ((type==2 && vector!=2) || (type==3 && !knownException(vector)) || (type==0 && vector<16)) return false;
    bool deliversError=(info&(1U<<11))!=0;
    bool needsError=type==3 && (vector==8 || (vector>=10 && vector<=14) || vector==17 || vector==21);
    if(deliversError!=needsError || (vector==8 && error)) return false;
    event=(uint64_t(info)&0x80000fffULL)|(deliversError?uint64_t(error)<<32:0);
    return true;
}
inline bool fromSVMEvent(uint64_t event,uint32_t &info,uint32_t &error) {
    info=error=0;
    if (!(event&(1ULL<<31))) return true;
    uint64_t check=0;
    uint32_t candidate=uint32_t(event);
    uint32_t candidateError=uint32_t(event>>32);
    if (!toSVMEvent(candidate,candidateError,check) || check!=event) return false;
    info=candidate;error=candidateError;return true;
}
}
#endif
