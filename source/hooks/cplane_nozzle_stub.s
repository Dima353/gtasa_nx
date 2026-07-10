/* cplane_nozzle_stub.s -- mid-function stub for Hydra manual nozzle control.
 *
 * Hooked into CPlane::ProcessControlInputs (libGame.so) at +0x5D4, just after the
 * `modelIndex == 520` (Hydra) check. It replaces the stock nozzle-input block
 * (+0x5D4..+0x784, which moves the nozzles from throttle + Accelerate/Brake) with
 * a call into CPlane__nozzle_manual (game.c), which rotates the nozzles from the
 * right-stick vertical axis instead. Rejoins the function at +0x784.
 *
 * At the hook site x19 = CPlane* (this) and w20 = the pad index (the function's
 * unsigned char argument). Both are callee-saved, so they survive the C call, as
 * does s8 (throttle, in callee-saved v8) which the +0x784 continuation still uses.
 * No caller-saved register is live across the hook (verified), and sp is inherited
 * 16-byte aligned, so we can call C directly without saving anything. Written in a
 * .s file for the same reasons as fov_stub.s.
 */

.section .text.CPlane__nozzle_stub, "ax", %progbits
.global CPlane__nozzle_stub
.type   CPlane__nozzle_stub, %function
.align  2
CPlane__nozzle_stub:
    mov  x0, x19                  // self = CPlane* (this)
    and  w1, w20, #0xff           // pad index (unsigned char argument)
    bl   CPlane__nozzle_manual
    adrp x8, cplane_nozzle_ret
    add  x8, x8, :lo12:cplane_nozzle_ret
    ldr  x8, [x8]                 // runtime address of ProcessControlInputs+0x784
    br   x8
.size CPlane__nozzle_stub, .-CPlane__nozzle_stub
