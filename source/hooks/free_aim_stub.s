/* free_aim_stub.s -- free-aim binding for CTaskSimplePlayerOnFoot::ProcessPlayerWeapon.
 *
 * During auto-aim (a lock-on target exists) the engine checks
 * CPad::ShiftTargetRightJustDown to cycle the target. We hook right after that
 * check (at +0x14ac, 0x68a6d0, the `tbz w0,#0,+0x158c`) so that when the target is
 * NOT being cycled, a C helper can test the free-aim button (D-pad Down) and, if
 * pressed, drop the lock into free aim (ClearWeaponTarget + set g_free_aim, which
 * MobileSettings::IsFreeAimMode then reports -- see game.c). The stub preserves the
 * original two outcomes: target cycling (reproduce the FindNextWeaponLockOnTarget
 * setup and rejoin at +0x1588) and the fall-through (rejoin at +0x158c).
 *
 * At the hook: w0 = ShiftTargetRightJustDown result, x19 = the CPlayerPed, x22 = the
 * pad. x19 is callee-saved so it survives the C call. free_aim_maybe lives in the
 * wrapper (same module) so a direct bl reaches it; the game rejoin addresses are
 * resolved in patch_game and loaded via adrp/:lo12: (hidden globals). x17 is dead on
 * entry (hook_arm64 trampoline) and used as scratch.
 */
.section .text.free_aim_stub, "ax", %progbits
.global free_aim_stub
.type   free_aim_stub, %function
.align  2
free_aim_stub:
    tbnz w0, #0, 1f              // target being cycled -> reproduce FindNext path
    stp  x29, x30, [sp, #-16]!   // else: check the free-aim button
    mov  x0, x19                 // playerPed
    bl   free_aim_maybe
    ldp  x29, x30, [sp], #16
    adrp x17, faim_ret_else      // rejoin at +0x158c (0x68a7b0)
    add  x17, x17, :lo12:faim_ret_else
    ldr  x17, [x17]
    br   x17
1:
    ldr  x1, [x19, #2272]        // reproduce: prevTarget = [playerPed+0x8e0]
    mov  x0, x19                 // playerPed
    mov  w2, wzr                 // 0
    adrp x17, faim_ret_shift     // rejoin at +0x1588 (0x68a7ac: bl FindNext...)
    add  x17, x17, :lo12:faim_ret_shift
    ldr  x17, [x17]
    br   x17
.size free_aim_stub, .-free_aim_stub

/* Trampoline to the real CPlayerPed::FindWeaponLockOnTarget so the C hook can call
 * it in normal (non-free-aim) mode. Reproduces the 4 prologue instrs and rejoins at
 * +0x10 (0x5b93d8). On arm64 that function re-acquires a lock even when
 * IsFreeAimMode is true (a GetInputType()==1 fallback), so the C hook no-ops it
 * while g_free_aim is set to keep the reticle free. findlock_cont is set in game.c. */
.section .text.FindWeaponLockOnTarget_orig, "ax", %progbits
.global FindWeaponLockOnTarget_orig
.type   FindWeaponLockOnTarget_orig, %function
.align  2
FindWeaponLockOnTarget_orig:
    sub  sp, sp, #0xb0
    stp  d13, d12, [sp, #32]
    stp  d11, d10, [sp, #48]
    stp  d9, d8, [sp, #64]
    adrp x17, findlock_cont
    add  x17, x17, :lo12:findlock_cont
    ldr  x17, [x17]
    br   x17
.size FindWeaponLockOnTarget_orig, .-FindWeaponLockOnTarget_orig
