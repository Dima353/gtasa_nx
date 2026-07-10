/* fov_stub.s -- mid-function stub for the emergency-vehicle / FOV fix.
 *
 * Hooked into CCamera::Process (libGame.so) where it computes the frustum scale
 * 70.0 / CDraw::ms_fFOV. We feed it the aspect-corrected fake_fov instead, so the
 * frustum matches widescreen and CEntity::GetIsOnScreen (which gates CCarCtrl
 * vehicle removal) stops culling approaching emergency vehicles.
 *
 * Replaces the 4 instructions at CCamera::Process+0xDB8 (ldr ms_fFOV; mov #70.0;
 * fmov; fdiv) and branches to +0xDC8. Written in a .s file because this aarch64
 * GCC ignores __attribute__((naked)) (a compiler prologue would corrupt sp since
 * we leave via br), and to reference the globals PIC-safely (they are hidden, so
 * adrp/add is allowed). fake_fov / ccamera_fov_ret are defined in game.c.
 */

.section .text.CCamera__Process_fov_stub, "ax", %progbits
.global CCamera__Process_fov_stub
.type   CCamera__Process_fov_stub, %function
.align  2
CCamera__Process_fov_stub:
    adrp x8, fake_fov
    add  x8, x8, :lo12:fake_fov
    ldr  s0, [x8]                 // s0 = fake_fov (aspect-corrected)
    movz w8, #0x428c, lsl #16     // w8 = 0x428c0000 = 70.0f
    fmov s1, w8
    fdiv s0, s1, s0               // s0 = 70.0 / fake_fov
    adrp x8, ccamera_fov_ret
    add  x8, x8, :lo12:ccamera_fov_ret
    ldr  x8, [x8]                 // x8 = runtime address of CCamera::Process+0xDC8
    br   x8
.size CCamera__Process_fov_stub, .-CCamera__Process_fov_stub
