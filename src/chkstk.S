# chkstk.S - stack probe used by the compiler for stack frames larger than one page
# (same algorithm as libgcc's ___chkstk_ms). Touches each page in order so Windows can grow the
# stack through its guard page. Preserves all registers; size to allocate is in rax.
.intel_syntax noprefix
.text
.globl ___chkstk_ms
.p2align 4
___chkstk_ms:
    push rcx
    push rax
    cmp rax, 0x1000
    lea rcx, [rsp + 24]
    jb 2f
1:  sub rcx, 0x1000
    or qword ptr [rcx], 0
    sub rax, 0x1000
    cmp rax, 0x1000
    ja 1b
2:  sub rcx, rax
    or qword ptr [rcx], 0
    pop rax
    pop rcx
    ret
