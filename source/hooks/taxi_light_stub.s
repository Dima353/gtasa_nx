/* taxi_light_stub.s -- trampoline to the original CAutomobile::Render.
 *
 * The taxi "for hire" roof light (removed on mobile) is restored by post-hooking
 * CAutomobile::Render (JPatch TaxiLights): call the original render, then for a
 * Taxi/Cabbie set the light on when it's an available cab. game.c's
 * CAutomobile__Render_hook is installed at Render's entry via hook_arm64; it calls
 * this trampoline to run the real Render. We reproduce Render's first 4 prologue
 * instructions (the 16 bytes hook_arm64 overwrote) and branch to Render+16. The
 * reproduced `stp x29,x30` saves the caller's return address, so the real Render's
 * epilogue `ret` returns into the C hook as normal. caut_render_cont is defined in
 * game.c (hidden global -> adrp/add stay PIC-valid, per fov_stub.s).
 */

.section .text.CAutomobile__Render_orig, "ax", %progbits
.global CAutomobile__Render_orig
.type   CAutomobile__Render_orig, %function
.align  2
CAutomobile__Render_orig:
    sub  sp, sp, #0xc0            // reproduce CAutomobile::Render +0x0
    stp  d9, d8, [sp, #80]        //                              +0x4
    stp  x29, x30, [sp, #96]      //                              +0x8
    stp  x28, x27, [sp, #112]     //                              +0xc
    adrp x17, caut_render_cont
    add  x17, x17, :lo12:caut_render_cont
    ldr  x17, [x17]              // runtime address of CAutomobile::Render+0x10
    br   x17
.size CAutomobile__Render_orig, .-CAutomobile__Render_orig
