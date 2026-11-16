// Single-access probes. Destination writes occur only after a successful source
// load. The trap adapter recognizes ONLY these exact faulting instructions.
#if defined(__x86_64__)
.text
.globl _amdhv_probe_read8
.globl _amdhv_probe_read8_fault
.globl _amdhv_probe_read8_recover
.p2align 4
_amdhv_probe_read8:
_amdhv_probe_read8_fault:
    movb (%rdi), %al
    movb %al, (%rsi)
    movl $1, %eax
    retq
_amdhv_probe_read8_recover:
    xorl %eax, %eax
    retq
.globl _amdhv_probe_write8
.globl _amdhv_probe_write8_fault
.globl _amdhv_probe_write8_recover
.p2align 4
_amdhv_probe_write8:
    movb (%rsi), %al
_amdhv_probe_write8_fault:
    movb %al, (%rdi)
    movl $1, %eax
    retq
_amdhv_probe_write8_recover:
    xorl %eax, %eax
    retq
.globl _amdhv_probe_write64
.globl _amdhv_probe_write64_fault
.globl _amdhv_probe_write64_recover
.p2align 4
_amdhv_probe_write64:
    movq (%rsi), %rax
_amdhv_probe_write64_fault:
    movq %rax, (%rdi)
    movl $1, %eax
    retq
_amdhv_probe_write64_recover:
    xorl %eax, %eax
    retq
#endif
