# fovstubs.S - widened field of view at the places that build the camera's projection and frusta
# (cull.cpp). Since 0.3.17 the camera object's own fov field (+0x274) keeps the game's value: the
# camera controller eases from it, and the model-detail code reads it. Writing 120 there (0.3.11 to
# 0.3.16) drifted the game's own value towards 120 and gave low-detail models.
#
# Each site is one 8-byte "movss xmmN, [reg+0x274]" that MinHook sends here: if reg is the main
# camera object (nvr_fov_site_obj[idx], 0 = off; per site since 0.3.22 for the Ctrl+Shift+F6
# test), xmmN gets nvr_fov_wide and execution resumes after the
# instruction; otherwise the original instruction runs (MinHook's trampoline). Flags are
# clobbered; the next game instructions (mulss / movss) don't read them.
.intel_syntax noprefix

.macro FOVSTUB name, reg, xmm, idx, val=nvr_fov_wide
.globl \name
.p2align 4
\name:
    cmp \reg, qword ptr [rip + nvr_fov_site_obj + 8 * \idx]
    jne 1f
    movss \xmm, dword ptr [rip + \val]
    lock inc qword ptr [rip + nvr_fov_hits + 8 * \idx]
    jmp qword ptr [rip + nvr_fov_resume + 8 * \idx]
1:  jmp qword ptr [rip + nvr_fov_orig + 8 * \idx]
.endm

# Calls a C function from the middle of game code: every volatile register (rax, rcx, rdx, r8-r11,
# xmm0-xmm5) is kept, the stack is realigned to 16 (rbx holds the old rsp; rbx is callee-saved).
# Arguments: rcx = r14, rdx = rbp, r8 = rsi (the frustum builder's eye / target / up).
.macro SAVED_CALL fn
    push rbx
    mov rbx, rsp
    and rsp, -16
    sub rsp, 192
    mov qword ptr [rsp + 32], rax
    mov qword ptr [rsp + 40], rcx
    mov qword ptr [rsp + 48], rdx
    mov qword ptr [rsp + 56], r8
    mov qword ptr [rsp + 64], r9
    mov qword ptr [rsp + 72], r10
    mov qword ptr [rsp + 80], r11
    movups xmmword ptr [rsp + 96], xmm0
    movups xmmword ptr [rsp + 112], xmm1
    movups xmmword ptr [rsp + 128], xmm2
    movups xmmword ptr [rsp + 144], xmm3
    movups xmmword ptr [rsp + 160], xmm4
    movups xmmword ptr [rsp + 176], xmm5
    mov rcx, r14
    mov rdx, rbp
    mov r8, rsi
    call \fn
    mov rax, qword ptr [rsp + 32]
    mov rcx, qword ptr [rsp + 40]
    mov rdx, qword ptr [rsp + 48]
    mov r8, qword ptr [rsp + 56]
    mov r9, qword ptr [rsp + 64]
    mov r10, qword ptr [rsp + 72]
    mov r11, qword ptr [rsp + 80]
    movups xmm0, xmmword ptr [rsp + 96]
    movups xmm1, xmmword ptr [rsp + 112]
    movups xmm2, xmmword ptr [rsp + 128]
    movups xmm3, xmmword ptr [rsp + 144]
    movups xmm4, xmmword ptr [rsp + 160]
    movups xmm5, xmmword ptr [rsp + 176]
    mov rsp, rbx
    pop rbx
.endm

.text
FOVSTUB nvr_fov_stub0, rbx, xmm1, 0   # camera update: projection rebuild   movss xmm1, [rbx+0x274]
# frustum builder args: also remember r14 / rbp / rsi (its rdx / r8 / r9 arguments: eye, target and
# up of a look-at) for the log. Head-direction culling (0.3.22, nvr_head_cull != 0): before the
# call, nvr_head_cull_builder(eye, target, up) may turn the look-at the way the headset looks. All
# volatile registers are kept (this is the middle of a game function).
.globl nvr_fov_stub1
.p2align 4
nvr_fov_stub1:
    mov qword ptr [rip + nvr_diag_regs], r14
    mov qword ptr [rip + nvr_diag_regs + 8], rbp
    mov qword ptr [rip + nvr_diag_regs + 16], rsi
    cmp rax, qword ptr [rip + nvr_fov_obj]
    jne nvr_fov_stub1b
    cmp dword ptr [rip + nvr_head_cull], 0
    je nvr_fov_stub1b
    SAVED_CALL nvr_head_cull_builder
    jmp nvr_fov_stub1b
FOVSTUB nvr_fov_stub1b, rax, xmm1, 1, nvr_fov_wide_b  # camera update: frustum builder args movss xmm1, [rax+0x274]
FOVSTUB nvr_fov_stub2, rax, xmm0, 2   # render thread copy (+0xd4)          movss xmm0, [rax+0x274]
FOVSTUB nvr_fov_stub3, rax, xmm0, 3   # render thread copy (+0xd4)          movss xmm0, [rax+0x274]

# Right after the frustum builder call returns (0.3.23): put the game's look-at back (the block is
# game data, 0.3.22 log), then the original "mov rbp, [rsp+0x90]" (MinHook trampoline).
.globl nvr_post_builder
.p2align 4
nvr_post_builder:
    SAVED_CALL nvr_head_cull_restore
    jmp qword ptr [rip + nvr_post_orig]

.data
.p2align 3
.globl nvr_fov_wide_b, nvr_post_orig, nvr_fov_obj, nvr_fov_site_obj, nvr_head_cull, nvr_fov_wide, nvr_fov_resume, nvr_fov_orig, nvr_fov_hits, nvr_diag_regs
nvr_diag_regs:  .quad 0, 0, 0
nvr_fov_obj:    .quad 0
nvr_post_orig:  .quad 0
nvr_fov_site_obj: .quad 0, 0, 0, 0
nvr_fov_resume: .quad 0, 0, 0, 0
nvr_fov_orig:   .quad 0, 0, 0, 0
nvr_fov_hits:   .quad 0, 0, 0, 0
nvr_fov_wide:   .long 0
nvr_fov_wide_b: .long 0   # the frustum builder's (smaller with head culling)
nvr_head_cull:  .long 0
