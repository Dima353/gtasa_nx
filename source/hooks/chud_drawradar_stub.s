/* chud_drawradar_stub.s -- entry detour for the radar alpha fix.
 *
 * Hooked at the entry of CHud::DrawRadar (libGame.so). The radar is a touch
 * widget whose alpha (widget+88) the game keeps faded on a gamepad; DrawRadar
 * reads that alpha for the map, frame and blips, so the minimap renders empty.
 * We force the widget alpha to 255 here, right before DrawRadar reads it (so the
 * game's per-frame animation can't override it), then run the real function.
 *
 * CHud::DrawRadar() is a static, argument-less void function, so x0-x18 are free
 * to clobber across the radar_setup() call. We preserve x29/x30 so DrawRadar's own
 * prologue saves the real return address, reproduce the 4 prologue instructions
 * that hook_arm64 overwrote, then jump to DrawRadar+16 (drawradar_cont). Written
 * in a .s file for the same reasons as the other stubs; radar_setup /
 * drawradar_cont are defined in game.c.
 */

.section .text.CHud__DrawRadar_stub, "ax", %progbits
.global CHud__DrawRadar_stub
.type   CHud__DrawRadar_stub, %function
.align  2
CHud__DrawRadar_stub:
    stp  x29, x30, [sp, #-16]!      // keep lr across the C call
    bl   radar_setup               // force radar widget alpha = 255
    ldp  x29, x30, [sp], #16
    // reproduce the 4 DrawRadar prologue instructions the hook overwrote:
    sub  sp, sp, #0x110
    stp  d15, d14, [sp, #144]
    stp  d13, d12, [sp, #160]
    stp  d11, d10, [sp, #176]
    // continue into DrawRadar past those 4 instructions (runtime addr = base+16)
    adrp x16, drawradar_cont
    add  x16, x16, :lo12:drawradar_cont
    ldr  x16, [x16]
    br   x16
.size CHud__DrawRadar_stub, .-CHud__DrawRadar_stub
