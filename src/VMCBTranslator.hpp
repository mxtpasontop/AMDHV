#ifndef AMDHV_VMCB_TRANSLATOR_HPP
#define AMDHV_VMCB_TRANSLATOR_HPP

#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>
#include <src/SoftVMCS.hpp>
#include <src/NPTManager.hpp>

namespace VMCBTranslator {

// Convert 32-bit Intel VMX Segment Access Rights to 16-bit AMD VMCB Segment Attributes
uint16_t intelARToAMDAttrib(uint32_t intelAR);

// Convert 16-bit AMD VMCB Segment Attributes to 32-bit Intel VMX Segment Access Rights
uint32_t amdAttribToIntelAR(uint16_t amdAttrib);

// Convert AMD SVM IOIO EXITINFO1 to Intel VMX Exit Qualification for I/O Instructions
uint64_t amdIOInfoToIntelIOQual(uint64_t ioExitInfo1);

enum class EntryResult { Ready, InvalidArgument, UnsupportedControls, InvalidEvent, InvalidMSRs, MissingNPT };
enum class ExitResult { GuestExit, Retry, HostEvent, FailedEntry, UnsupportedExit, MemoryFailure, InvalidArgument, InvalidEvent };
struct GuestMSRs { size_t count {}; hv_vmx_msr_entry_t entries[32] {}; };
// Synchronize all guest state & control fields from SoftVMCS (+ AppleHV vCPU MSRs/GPRs) into AMD VMCB before VMRUN
[[nodiscard]] EntryResult syncVMCStoVMCB(SoftVMCS::SoftVMCSInstance *vmcs, AppleHVGPRFrame *gprs, GuestMSRs *msrs = nullptr, const NPTManager::Lease *root = nullptr);

// Synchronize all guest state & exit reason/qualification from AMD VMCB back into SoftVMCS (+ AppleHV vCPU MSRs/GPRs) after VMEXIT.
// Sets outFastPathRetry = true if an #NPF exit was transparently resolved by syncing an already-valid EPT PTE into the shadow NPT.
[[nodiscard]] ExitResult syncVMCBtoVMCS(SoftVMCS::SoftVMCSInstance *vmcs, AppleHVGPRFrame *gprs, GuestMSRs *msrs, bool &outFastPathRetry);

} // namespace VMCBTranslator

#endif // AMDHV_VMCB_TRANSLATOR_HPP
