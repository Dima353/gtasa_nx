/* mainmenu_exit_stub.s -- trampoline to the original MainMenuScreen::AddAllItems.
 *
 * The pause menu has no Exit/Quit item (its AddAllItems builds Resume/Settings/...
 * but never one bound to MainMenuScreen::OnExit, which we hook to save+quit). To
 * add one we hook AddAllItems with a C wrapper (game.c) that runs the original,
 * then appends a "Quit" item. This stub IS the "run the original": it reproduces
 * the 4 prologue instructions the entry hook overwrote and jumps to AddAllItems+16,
 * so a `bl` here executes the whole real function and returns to the C wrapper.
 *
 * Called with x0 = the MainMenuScreen `this` (AddAllItems takes only that). The
 * reproduced prologue reads [x0+21], so x0 must be live -- the wrapper passes it.
 * addallitems_cont (game.c) holds the runtime address of AddAllItems+16.
 */

.section .text.MainMenuScreen__AddAllItems_orig, "ax", %progbits
.global MainMenuScreen__AddAllItems_orig
.type   MainMenuScreen__AddAllItems_orig, %function
.align  2
MainMenuScreen__AddAllItems_orig:
    stp  x29, x30, [sp, #-32]!   // reproduce AddAllItems prologue (0x70e8a4..0x70e8b0)
    str  x19, [sp, #16]
    mov  x29, sp
    ldrb w8, [x0, #21]
    adrp x16, addallitems_cont   // continue in the real function past those 4 instrs
    add  x16, x16, :lo12:addallitems_cont
    ldr  x16, [x16]
    br   x16
.size MainMenuScreen__AddAllItems_orig, .-MainMenuScreen__AddAllItems_orig
