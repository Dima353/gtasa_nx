/* mobilemenu_update_stub.s -- trampoline to the original MobileMenu::Update.
 *
 * The pause-menu Start/Select fix pre-hooks MobileMenu::Update: game.c's
 * MobileMenu__Update_hook is installed at Update's entry via hook_arm64; it routes
 * the '+'/'-' presses (resume / open map) when a menu is open, then calls this
 * trampoline to run the real Update. We reproduce Update's first 4 prologue
 * instructions (the 16 bytes hook_arm64 overwrote) and branch to Update+16. The
 * reproduced `stp x29,x30` saves the caller's return address so the real Update's
 * `ret` returns into the C hook. mm_update_cont is defined in game.c.
 */

.section .text.MobileMenu__Update_orig, "ax", %progbits
.global MobileMenu__Update_orig
.type   MobileMenu__Update_orig, %function
.align  2
MobileMenu__Update_orig:
    sub  sp, sp, #0xc0            // reproduce MobileMenu::Update +0x0
    stp  d13, d12, [sp, #64]      //                             +0x4
    stp  d11, d10, [sp, #80]      //                             +0x8
    stp  d9, d8, [sp, #96]        //                             +0xc
    adrp x17, mm_update_cont
    add  x17, x17, :lo12:mm_update_cont
    ldr  x17, [x17]             // runtime address of MobileMenu::Update+0x10
    br   x17
.size MobileMenu__Update_orig, .-MobileMenu__Update_orig
