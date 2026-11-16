#ifndef AMDHV_SVM_TRANSITION_HPP
#define AMDHV_SVM_TRANSITION_HPP
#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>
#include <src/TransitionLayout.h>
namespace SVMTransition {
struct __attribute__((packed)) IDTR {uint16_t limit {};uint64_t base {};uint8_t pad[6] {};};
struct __attribute__((packed)) Gate {uint16_t low {},selector {};uint8_t ist {},type {};uint16_t middle {};uint32_t high {},reserved {};};
struct alignas(4096) Context {
    VMCB *guestVA {};uint64_t guestPA {},runPA {},hostPA {};
    AppleHVGPRFrame *frame {};
    uint64_t cr2 {},dr6 {},dr7 {},flags {};
    IDTR hostIDTR,privateIDTR;
    int64_t status {};uint64_t vector {},faultRIP {},error {},faultAddress {},vmrunReturned {},ready {};
    uint8_t padding[SVM_CTX_IDT-160] {};
    Gate idt[256];
};
static_assert(sizeof(IDTR)==16 && sizeof(Gate)==16 && sizeof(Context)==SVM_CTX_SIZE,"transition storage layout");
#define AMDHV_TRANSITION_OFFSET(field,offset) static_assert(offsetof(Context,field)==offset,"transition " #field " offset")
AMDHV_TRANSITION_OFFSET(guestVA,SVM_CTX_GUEST_VA);AMDHV_TRANSITION_OFFSET(guestPA,SVM_CTX_GUEST_PA);
AMDHV_TRANSITION_OFFSET(runPA,SVM_CTX_RUN_PA);AMDHV_TRANSITION_OFFSET(hostPA,SVM_CTX_HOST_PA);
AMDHV_TRANSITION_OFFSET(frame,SVM_CTX_FRAME);AMDHV_TRANSITION_OFFSET(cr2,SVM_CTX_CR2);
AMDHV_TRANSITION_OFFSET(dr6,SVM_CTX_DR6);AMDHV_TRANSITION_OFFSET(dr7,SVM_CTX_DR7);
AMDHV_TRANSITION_OFFSET(flags,SVM_CTX_FLAGS);AMDHV_TRANSITION_OFFSET(hostIDTR,SVM_CTX_HOST_IDTR);
AMDHV_TRANSITION_OFFSET(privateIDTR,SVM_CTX_PRIVATE_IDTR);AMDHV_TRANSITION_OFFSET(status,SVM_CTX_STATUS);
AMDHV_TRANSITION_OFFSET(vector,SVM_CTX_VECTOR);AMDHV_TRANSITION_OFFSET(faultRIP,SVM_CTX_FAULT_RIP);
AMDHV_TRANSITION_OFFSET(error,SVM_CTX_ERROR);AMDHV_TRANSITION_OFFSET(faultAddress,SVM_CTX_FAULT_ADDRESS);
AMDHV_TRANSITION_OFFSET(vmrunReturned,SVM_CTX_RETURNED);AMDHV_TRANSITION_OFFSET(ready,SVM_CTX_READY);
AMDHV_TRANSITION_OFFSET(idt,SVM_CTX_IDT);
#undef AMDHV_TRANSITION_OFFSET
// Storage, both VMCBs and frame must be wired, disjoint, and owned for the whole
// CPU run lease. GIF starts set; the CPU/MSR owner prevents recursive transitions.
// Context is per CPU, never a kernel stack temporary. No FP/XSTATE instructions:
// the verified outer AppleHV FP/debug ownership must be retained.
bool initialize(Context &context,VMCB *guestVA,uint64_t guestPA,uint64_t hostAuxPA,AppleHVGPRFrame *frame,uint16_t kernelCS);
enum class Result : int { Exit=0, RecoveredFault=-1, FatalEvent=-2, Unavailable=-3 };
[[nodiscard]] Result run(Context &context);
}
// Internal privileged primitive, called only by the accepted kernel wrapper or
// the freestanding software-emulation harness. Ordinary userspace never calls it.
extern "C" int amdhv_svm_transition_raw(SVMTransition::Context *);
extern "C" const int32_t amdhv_svm_trap_offsets[256];
#endif
