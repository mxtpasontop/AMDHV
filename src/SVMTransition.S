#include "TransitionLayout.h"
#ifdef __APPLE__
#define SVM_SYMBOL(n) _##n
#else
#define SVM_SYMBOL(n) n
#endif
.text
.p2align 4
.globl SVM_SYMBOL(amdhv_svm_transition_raw)
SVM_SYMBOL(amdhv_svm_transition_raw):
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $32,%rsp
    movq %rdi,0(%rsp)
    movq SVM_CTX_FRAME(%rdi),%rax
    movq %rax,8(%rsp)
    movq $0,16(%rsp)           // bit 0: GIF cleared, bit 1: host auxiliary saved
    movq $-1,24(%rsp)
    movq $0,SVM_CTX_RETURNED(%rdi)
    movq $0,SVM_CTX_VECTOR(%rdi)
    movq $0,SVM_CTX_FAULT_RIP(%rdi)
    movq $0,SVM_CTX_ERROR(%rdi)
    movq $0,SVM_CTX_FAULT_ADDRESS(%rdi)
    pushfq
    popq %rax
    movq %rax,SVM_CTX_FLAGS(%rdi)
    testq $0x100,%rax          // Host single-step cannot cross the private window.
    jnz .Lsvm_epilogue
    cli
    movq %cr2,%rax
    movq %rax,SVM_CTX_CR2(%rdi)
    movq %dr6,%rax
    movq %rax,SVM_CTX_DR6(%rdi)
    movq %dr7,%rax
    movq %rax,SVM_CTX_DR7(%rdi)
    sidt SVM_CTX_HOST_IDTR(%rdi)
    lidt SVM_CTX_PRIVATE_IDTR(%rdi)
.Lsvm_clgi:
    clgi
    orq $1,16(%rsp)
    movq SVM_CTX_HOST_PA(%rdi),%rax
.Lsvm_save_host:
    vmsave %rax
    orq $2,16(%rsp)
    movq SVM_CTX_GUEST_PA(%rdi),%rax
.Lsvm_load_guest:
    vmload %rax
    // No GS-relative operand, C++ call, changing stack pointer or original-IDT
    // dispatch is permitted until the host auxiliary state is restored.
    movq 8(%rsp),%rax
    movq 0x18(%rax),%rcx
    movq 0x20(%rax),%rdx
    movq 0x28(%rax),%rbx
    movq 0x30(%rax),%rsi
    movq 0x38(%rax),%rdi
    movq 0x48(%rax),%rbp
    movq 0x50(%rax),%r8
    movq 0x58(%rax),%r9
    movq 0x60(%rax),%r10
    movq 0x68(%rax),%r11
    movq 0x70(%rax),%r12
    movq 0x78(%rax),%r13
    movq 0x80(%rax),%r14
    movq 0x88(%rax),%r15
    movq 0(%rsp),%rax
    movq SVM_CTX_RUN_PA(%rax),%rax
    // V_INTR_MASKING uses the host IF sampled by VMRUN. It must be set even
    // when our caller entered with IF=0, or an IF=0 guest blocks cancellation.
    // GIF remains clear, and STI's shadow covers the immediately following run.
    sti
.Lsvm_run:
    vmrun %rax
    cli
    movq 8(%rsp),%rax
    movq %rcx,0x18(%rax)
    movq %rdx,0x20(%rax)
    movq %rbx,0x28(%rax)
    movq %rsi,0x30(%rax)
    movq %rdi,0x38(%rax)
    movq %rbp,0x48(%rax)
    movq %r8,0x50(%rax)
    movq %r9,0x58(%rax)
    movq %r10,0x60(%rax)
    movq %r11,0x68(%rax)
    movq %r12,0x70(%rax)
    movq %r13,0x78(%rax)
    movq %r14,0x80(%rax)
    movq %r15,0x88(%rax)
    movq 0(%rsp),%rdi
    movq $1,SVM_CTX_RETURNED(%rdi)
    movq $0,24(%rsp)
    movq SVM_CTX_GUEST_PA(%rdi),%rax
.Lsvm_save_guest:
    vmsave %rax
.Lsvm_restore:
    movq 0(%rsp),%rdi
    testq $2,16(%rsp)
    jz .Lsvm_restored
    // The saved host page is dedicated, wired and inaccessible to the guest.
    // A fault while restoring that trusted page is an unrecoverable invariant
    // failure; the nested handler stops this CPU rather than using guest GS.
    orq $4,16(%rsp)
    movq SVM_CTX_HOST_PA(%rdi),%rax
.Lsvm_load_host:
    vmload %rax
    andq $-5,16(%rsp)
.Lsvm_restored:
    movq SVM_CTX_CR2(%rdi),%rax
    movq %rax,%cr2
    movq SVM_CTX_DR6(%rdi),%rax
    movq %rax,%dr6
    movq SVM_CTX_DR7(%rdi),%rax
    movq %rax,%dr7
    lidt SVM_CTX_HOST_IDTR(%rdi)
    testq $1,16(%rsp)
    jz .Lsvm_epilogue
    stgi
.Lsvm_epilogue:
    movq 0(%rsp),%rdi
    movq SVM_CTX_FLAGS(%rdi),%r11
    movq 24(%rsp),%rax
    movq %rax,SVM_CTX_STATUS(%rdi)
    addq $32,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    pushq %r11
    popfq
    retq

// All private gates have IST=0 and the current kernel CS. VMLOAD changes hidden
// TR/GS state, so using the original IDT's IST before restoring host TR is unsafe.
// The long-mode CPU frame provides the interrupted RSP independently of GS.
.Lsvm_exception:
    movq 40(%rsp),%r10
    movq 0(%r10),%rdi
    testq $4,16(%r10)
    jnz .Lsvm_unrecoverable
    movq 0(%rsp),%rax
    movq %rax,SVM_CTX_VECTOR(%rdi)
    movq 8(%rsp),%rax
    movq %rax,SVM_CTX_ERROR(%rdi)
    movq 16(%rsp),%rcx
    movq %rcx,SVM_CTX_FAULT_RIP(%rdi)
    movq %cr2,%rax
    movq %rax,SVM_CTX_FAULT_ADDRESS(%rdi)
    movq $-2,24(%r10)
    movq 0(%rsp),%rax
    cmpq $6,%rax
    je .Lsvm_check_site
    cmpq $13,%rax
    je .Lsvm_check_site
    cmpq $14,%rax
    jne .Lsvm_recover
.Lsvm_check_site:
    leaq .Lsvm_clgi(%rip),%rax
    cmpq %rax,%rcx
    je .Lsvm_expected
    leaq .Lsvm_save_host(%rip),%rax
    cmpq %rax,%rcx
    je .Lsvm_expected
    leaq .Lsvm_load_guest(%rip),%rax
    cmpq %rax,%rcx
    je .Lsvm_expected
    leaq .Lsvm_run(%rip),%rax
    cmpq %rax,%rcx
    je .Lsvm_expected
    leaq .Lsvm_save_guest(%rip),%rax
    cmpq %rax,%rcx
    jne .Lsvm_recover
.Lsvm_expected:
    movq $-1,24(%r10)
.Lsvm_recover:
    movq %r10,%rsp
    jmp .Lsvm_restore
.Lsvm_unrecoverable:
    cli
    hlt
    jmp .Lsvm_unrecoverable

.altmacro
.macro SVM_VECTOR number
.Lsvm_vector_\number:
    .if (\number!=8)&&(\number!=10)&&(\number!=11)&&(\number!=12)&&(\number!=13)&&(\number!=14)&&(\number!=17)&&(\number!=21)&&(\number!=29)&&(\number!=30)
    pushq $0
    .endif
    pushq $\number
    jmp .Lsvm_exception
.endm
.set svm_vector,0
.rept 256
SVM_VECTOR %svm_vector
.set svm_vector,svm_vector+1
.endr
.p2align 2
.globl SVM_SYMBOL(amdhv_svm_trap_offsets)
SVM_SYMBOL(amdhv_svm_trap_offsets):
.macro SVM_VECTOR_OFFSET number
    .long .Lsvm_vector_\number - SVM_SYMBOL(amdhv_svm_trap_offsets)
.endm
.set svm_vector,0
.rept 256
SVM_VECTOR_OFFSET %svm_vector
.set svm_vector,svm_vector+1
.endr
