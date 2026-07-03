.section .interp,"a",@progbits
.string "/lib64/ld-linux-x86-64.so.2"

.text
.global _start
.type _start,@function
_start:
    xor %ebp, %ebp
    mov %rdx, %r9
    pop %rsi
    mov %rsp, %rdx
    andq $-16, %rsp
    pushq %rax
    pushq %rsp
    xor %r8d, %r8d
    xor %ecx, %ecx
    mov main@GOTPCREL(%rip), %rdi
    call __libc_start_main@PLT
    hlt
