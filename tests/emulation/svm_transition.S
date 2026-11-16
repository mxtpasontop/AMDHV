// Freestanding test image. Runs exclusively in QEMU TCG, never Hypervisor.framework.
#include "TransitionLayout.h"
#define HOST_ROOT 0x200000
#define HOST_PDPT 0x201000
#define HOST_PD 0x202000
#define HSAVE 0x210000
#define GUEST 0x211000
#define HOST_AUX 0x212000
#define CTX 0x214000
#define FRAME 0x216000
#define HOST_IDT 0x217000
#define NPT_ROOT 0x220000
#define NPT_PDPT 0x221000
#define NPT_PD 0x222000
#define GUEST_CODE 0x230000
#define HOST_GS 0x240000
#define HOST_KGS 0x241000
#define HOST_FS 0x242000
#define HOST_TSS 0x243000
#define MAILBOX 0x244000
.text
.code32
.p2align 2
.Lmultiboot:
    .long 0x1badb002,0x10003,-(0x1badb002+0x10003)
    .long .Lmultiboot,.Limage_start,.Limage_end,.Limage_end,.Lboot
.set .Limage_start,.Lmultiboot
.Lboot:
    cli
    cld
    movl $0x2ff000,%esp
    movl $0x200000,%edi
    xorl %eax,%eax
    movl $(0x50000/4),%ecx
    rep stosl
    movl $(HOST_PDPT|3),HOST_ROOT
    movl $(HOST_PD|3),HOST_PDPT
    movl $(NPT_PDPT|7),NPT_ROOT
    movl $(NPT_PD|7),NPT_PDPT
    xorl %ecx,%ecx
.Lpages:
    movl %ecx,%eax
    shll $21,%eax
    orl $0x87,%eax
    movl %eax,HOST_PD(,%ecx,8)
    movl %eax,NPT_PD(,%ecx,8)
    incl %ecx
    cmpl $64,%ecx
    jne .Lpages
    movl $HOST_ROOT,%eax
    movl %eax,%cr3
    movl %cr4,%eax
    orl $0x20,%eax
    movl %eax,%cr4
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    lgdt .Lgdt_pointer
    movl %cr0,%eax
    orl $0x80000001,%eax
    movl %eax,%cr0
    .byte 0xea
    .long .Llong_mode
    .word 8
.code64
.Llong_mode:
    movw $16,%ax
    movw %ax,%ss
    movw %ax,%ds
    movw %ax,%es
    xorw %ax,%ax
    movw %ax,%fs
    movw %ax,%gs
    movw $24,%ax
    ltr %ax
    movq $0x2ff000,HOST_TSS+4
    movq $0x2df000,HOST_TSS+36
    movq $0x2ff000,%rsp
    movb $0xff,%al
    outb %al,$0x21
    outb %al,$0xa1
    movq $HOST_IDT,%rdi
    movq $.Lhost_trap,%rax
    call .Lfill_gates
    movq $(HOST_IDT+32*16),%rdi
    movq $.Ltimer_interrupt,%rax
    movw %ax,0(%rdi)
    shrq $16,%rax
    movw %ax,6(%rdi)
    shrq $16,%rax
    movl %eax,8(%rdi)
    movb $1,4(%rdi) // The original timer gate uses the host TSS IST1.
    lidt .Lhost_idtr
    movq $.Lbegin_message,%rsi
    call .Lprint
    // Give host FS/GS/KERNEL_GS different bases from the guest's unmapped bases.
    movabsq $0x1122334455667788,%rax
    movq %rax,HOST_GS
    movl $0xc0000101,%ecx
    movl $HOST_GS,%eax
    xorl %edx,%edx
    wrmsr
    movl $0xc0000102,%ecx
    movl $HOST_KGS,%eax
    wrmsr
    movl $0xc0000100,%ecx
    movl $HOST_FS,%eax
    wrmsr
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x1000,%eax
    wrmsr
    movl $0xc0010117,%ecx
    movl $HSAVE,%eax
    xorl %edx,%edx
    wrmsr
    movq $0x12345678,%rax
    movq %rax,%cr2
    movq $0xffff0ff0,%rax
    movq %rax,%dr6
    movq $0x400,%rax
    movq %rax,%dr7
    stgi
    // Private context points only to test-owned identity-mapped RAM.
    movq $GUEST,CTX+SVM_CTX_GUEST_VA
    movq $GUEST,CTX+SVM_CTX_GUEST_PA
    movq $GUEST,CTX+SVM_CTX_RUN_PA
    movq $HOST_AUX,CTX+SVM_CTX_HOST_PA
    movq $FRAME,CTX+SVM_CTX_FRAME
    movw $4095,CTX+SVM_CTX_PRIVATE_IDTR
    movq $(CTX+SVM_CTX_IDT),CTX+SVM_CTX_PRIVATE_IDTR+2
    leaq amdhv_svm_trap_offsets(%rip),%rsi
    movq $(CTX+SVM_CTX_IDT),%rdi
    xorl %ecx,%ecx
.Lprivate_gates:
    movslq (%rsi,%rcx,4),%rax
    addq %rsi,%rax
    movw %ax,0(%rdi)
    movw $8,2(%rdi)
    movb $0,4(%rdi)
    movb $0x8e,5(%rdi)
    shrq $16,%rax
    movw %ax,6(%rdi)
    shrq $16,%rax
    movl %eax,8(%rdi)
    movl $0,12(%rdi)
    addq $16,%rdi
    incl %ecx
    cmpl $256,%ecx
    jne .Lprivate_gates
    movl $0x0100000f,GUEST+0xc
    movl $1,GUEST+0x10
    movl $1,GUEST+0x58
    movl $0x1000000,GUEST+0x60
    movb $1,GUEST+0x5c
    movq $1,GUEST+0x90
    movq $NPT_ROOT,GUEST+0xb0
    // Protected 32-bit guest. Segment bases are independent of host descriptors.
    movq $(GUEST+0x400),%rdi
    movl $6,%ecx
.Lguest_segments:
    movw $16,0(%rdi)
    movw $0xc93,2(%rdi)
    movl $-1,4(%rdi)
    addq $16,%rdi
    loop .Lguest_segments
    movw $8,GUEST+0x410
    movw $0xc9b,GUEST+0x412
    movl $0xf0000000,GUEST+0x448
    movl $0xf1234000,GUEST+0x458
    movl $0xf4567000,GUEST+0x620
    movw $24,GUEST+0x490
    movw $0x8b,GUEST+0x492
    movl $0x67,GUEST+0x494
    movl $0xf2345000,GUEST+0x498
    movq $0x1800,GUEST+0x4d0
    movq $0x31,GUEST+0x558
    movq $0x400,GUEST+0x560
    movl $0xffff0ff0,GUEST+0x568
    movq $2,GUEST+0x570
    movq $GUEST_CODE,GUEST+0x578
    movq $0x2ef000,GUEST+0x5d8
    movq $0xaa,GUEST+0x5f8
    movl $0xdeadbeef,GUEST+0x640
    movabsq $0x0007040600070406,%rax
    movq %rax,GUEST+0x668
    movq $.Lguest_payload,%rsi
    movq $GUEST_CODE,%rdi
    movq $(.Lguest_end-.Lguest_payload),%rcx
    rep movsb
    movabsq $0x9988776655443322,%rbx
    movabsq $0xaabbccddeeff0011,%r12
    movq $0x12,FRAME+0x70
    movq $0x13,FRAME+0x78
    movq $0x14,FRAME+0x80
    movq $0x15,FRAME+0x88
    movq $0xb1,FRAME+0x48
    movq $0xd1,FRAME+0x38
    movq $0x51,FRAME+0x30
    sti
    movq $1,MAILBOX
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    testl %eax,%eax
    jne .Lfail
    cmpq $0x78,GUEST+0x70
    jne .Lfail
    cmpq $0x1234,FRAME+0x28
    jne .Lfail
    cmpq $0x2345,FRAME+0x18
    jne .Lfail
    cmpq $0x4567,GUEST+0x5f8
    jne .Lfail
    cmpq $0x12,FRAME+0x70
    jne .Lfail
    cmpq $0x13,FRAME+0x78
    jne .Lfail
    cmpq $0x14,FRAME+0x80
    jne .Lfail
    cmpq $0x15,FRAME+0x88
    jne .Lfail
    cmpq $0xb1,FRAME+0x48
    jne .Lfail
    cmpq $0xd1,FRAME+0x38
    jne .Lfail
    cmpq $0x51,FRAME+0x30
    jne .Lfail
    call .Lcheck_host
    // Fault VMRUN after VMLOAD has installed the guest's unmapped GS base.
    movq $2,MAILBOX
    movq $(GUEST+1),CTX+SVM_CTX_RUN_PA
    movq $0xabcd,FRAME+0x28
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    cmpl $-1,%eax
    jne .Lfail
    cmpq $13,CTX+SVM_CTX_VECTOR
    jne .Lfail
    cmpq $0,CTX+SVM_CTX_RETURNED
    jne .Lfail
    cmpq $0xabcd,FRAME+0x28
    jne .Lfail
    call .Lcheck_host
    movq $GUEST,CTX+SVM_CTX_RUN_PA
    // Invalid guest state returns SVM's failed-entry exit, not a CPU exception.
    movq $3,MAILBOX
    movl $0,GUEST+0x58
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    testl %eax,%eax
    jne .Lfail
    // QEMU 9.2 zero-extends its uint32 error code; do not treat that as AMD ABI evidence.
    cmpl $-1,GUEST+0x70
    jne .Lfail
    call .Lcheck_host
    movl $1,GUEST+0x58
    // Invalid VMLOAD PA recovers before entry too.
    movq $4,MAILBOX
    movq $(GUEST+1),CTX+SVM_CTX_GUEST_PA
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    cmpl $-1,%eax
    jne .Lfail
    cmpq $13,CTX+SVM_CTX_VECTOR
    jne .Lfail
    call .Lcheck_host
    movq $GUEST,CTX+SVM_CTX_GUEST_PA
    // CLGI #UD with SVM disabled: no auxiliary load or STGI may be attempted.
    movq $5,MAILBOX
    movl $0xc0000080,%ecx
    rdmsr
    andl $0xffffefff,%eax
    wrmsr
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    cmpl $-1,%eax
    jne .Lfail
    cmpq $6,CTX+SVM_CTX_VECTOR
    jne .Lfail
    call .Lcheck_host
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x1000,%eax
    wrmsr
    // Revoked NPT mapping forces a real nested-page fault on instruction fetch.
    movq $6,MAILBOX
    movq $0,NPT_PD+8
    movb $1,GUEST+0x5c
    movq $GUEST_CODE,GUEST+0x578
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    testl %eax,%eax
    jne .Lfail
    cmpq $0x400,GUEST+0x70
    jne .Lfail
    call .Lcheck_host
    movq $0x200087,NPT_PD+8
    // A running guest with IF=0 must still exit for a physical host timer.
    // Test with incoming host IF=0, which is the important kernel call case.
    movq $8,MAILBOX
    cli
    call .Lstart_timer
    movq $0,MAILBOX+32
    movq $(GUEST_CODE+.Lguest_loop-.Lguest_payload),GUEST+0x578
    movb $1,GUEST+0x5c
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    testl %eax,%eax
    jne .Lfail
    cmpq $0x60,GUEST+0x70
    jne .Lfail
    pushfq
    popq %rax
    testq $0x200,%rax
    jnz .Lfail
    sti
    movl $1000000,%ecx
.Lawait_timer:
    cmpq $0,MAILBOX+32
    jne .Ltimer_done
    loop .Lawait_timer
    jmp .Lfail
.Ltimer_done:
    movb $0xff,%al
    outb %al,$0x21
    call .Lcheck_host
    // Reuse an ASID/root repeatedly while checking every restoration boundary.
    movq $7,MAILBOX
    movq $1000,MAILBOX+8
.Lrepeat:
    movb $1,GUEST+0x5c
    movq $GUEST_CODE,GUEST+0x578
    movq $CTX,%rdi
    call amdhv_svm_transition_raw
    testl %eax,%eax
    jne .Lfail
    cmpq $0x78,GUEST+0x70
    jne .Lfail
    call .Lcheck_host
    decq MAILBOX+8
    jnz .Lrepeat
    movq $.Lpass_message,%rsi
    call .Lprint
    xorl %eax,%eax
    movw $0xf4,%dx
    outl %eax,%dx
.Lhalt:
    cli
    hlt
    jmp .Lhalt
.Lcheck_host:
    movabsq $0x9988776655443322,%r11
    cmpq %r11,%rbx
    jne .Lfail
    movabsq $0xaabbccddeeff0011,%r11
    cmpq %r11,%r12
    jne .Lfail
    movq %gs:0,%rax
    movabsq $0x1122334455667788,%r11
    cmpq %r11,%rax
    jne .Lfail
    movl $0xc0000100,%ecx
    rdmsr
    cmpl $HOST_FS,%eax
    jne .Lfail
    movl $0xc0000102,%ecx
    rdmsr
    cmpl $HOST_KGS,%eax
    jne .Lfail
    movq %cr2,%rax
    cmpq $0x12345678,%rax
    jne .Lfail
    movq %dr7,%rax
    cmpq $0x400,%rax
    jne .Lfail
    movq %dr6,%rax
    movl $0xffff0ff0,%r11d
    cmpq %r11,%rax
    jne .Lfail
    str %ax
    cmpw $24,%ax
    jne .Lfail
    pushfq
    popq %rax
    testq $0x200,%rax
    jz .Lfail
    sidt MAILBOX+16
    cmpw $4095,MAILBOX+16
    jne .Lfail
    cmpq $HOST_IDT,MAILBOX+18
    jne .Lfail
    retq
.Lfill_gates:
    movl $256,%ecx
.Lfill_gate:
    movq %rax,%rdx
    movw %dx,0(%rdi)
    movw $8,2(%rdi)
    movb $0,4(%rdi)
    movb $0x8e,5(%rdi)
    shrq $16,%rdx
    movw %dx,6(%rdi)
    shrq $16,%rdx
    movl %edx,8(%rdi)
    movl $0,12(%rdi)
    addq $16,%rdi
    loop .Lfill_gate
    retq
.Lstart_timer:
    movb $0x11,%al
    outb %al,$0x20
    outb %al,$0xa0
    movb $0x20,%al
    outb %al,$0x21
    movb $0x28,%al
    outb %al,$0xa1
    movb $4,%al
    outb %al,$0x21
    movb $2,%al
    outb %al,$0xa1
    movb $1,%al
    outb %al,$0x21
    outb %al,$0xa1
    movb $0xff,%al
    outb %al,$0xa1
    movb $0x34,%al
    outb %al,$0x43
    movb $0xa9,%al
    outb %al,$0x40
    movb $4,%al
    outb %al,$0x40
    movb $0xfe,%al
    outb %al,$0x21
    retq
.Ltimer_interrupt:
    pushq %rax
    incq MAILBOX+32
    movb $0x20,%al
    outb %al,$0x20
    popq %rax
    iretq
.Lprint:
    lodsb
    testb %al,%al
    jz .Lprinted
    outb %al,$0xe9
    jmp .Lprint
.Lprinted:
    retq
.Lhost_trap:
    movq $.Lhost_trap_message,%rsi
    call .Lprint
.Lfail:
    movq $.Lfail_message,%rsi
    call .Lprint
    movb MAILBOX,%al
    addb $'0',%al
    outb %al,$0xe9
    movb $10,%al
    outb %al,$0xe9
    movl $1,%eax
    movw $0xf4,%dx
    outl %eax,%dx
    jmp .Lhalt
.Lbegin_message: .asciz "AMDHV emulated SVM transition tests\n"
.Lpass_message: .asciz "PASS: HLT, failed entry, VMLOAD/VMRUN/CLGI recovery, NPF, host timer cancellation with IF=0 and 1000 restores\n"
.Lfail_message: .asciz "FAIL case "
.Lhost_trap_message: .asciz "Unexpected host trap\n"
.p2align 3
.Lgdt:
    .quad 0,0x00af9a000000ffff,0x00cf92000000ffff
    .quad (0x67 | (0x3000<<16) | (0x24<<32) | (0x89<<40)),0
.Lgdt_end:
.Lgdt_pointer: .word .Lgdt_end-.Lgdt-1
    .long .Lgdt
.Lhost_idtr: .word 4095
    .quad HOST_IDT
.code32
.Lguest_payload:
    movl $0x1234,%ebx
    movl $0x2345,%ecx
    movl $0x3456,%edx
    movl $0x4567,%eax
    hlt
.Lguest_loop:
    jmp .Lguest_loop
.Lguest_end:
.code64
#include "SVMTransition.S"
.Limage_end:
