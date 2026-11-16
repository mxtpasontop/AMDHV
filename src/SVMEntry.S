// No fault-recoverable, build-verified XNU transition adapter exists yet.
// Returning failure preserves every host register, GIF, MSR and debug state.
// A future implementation must pass the recovery/state-restoration acceptance
// matrix before replacing this stub; the C++ acceptance interlock also stays shut.
#if defined(__x86_64__)
.text
.globl _amd_svm_vcpu_run
.align 4
_amd_svm_vcpu_run:
    movl $-1, %eax
    retq
#endif
