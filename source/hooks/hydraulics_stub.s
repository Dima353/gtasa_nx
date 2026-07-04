/* hydraulics_stub.s -- mid-function stubs for the hydraulics camera fix.
 *
 * In a lowrider with hydraulics the right analog stick tilts the car and the
 * camera is locked. This lets R3 toggle a "camera mode" (g_hydraulics_camera,
 * defined in game.c; default 0 = stock tilt) in which the stick moves the camera
 * instead. Three stubs, all gated on that flag:
 *
 *  - hyd_freeze_stub: hooked at CAutomobile::HydraulicControl+0x4cc (0x6a2458).
 *    When camera mode is on, GetHydraulicJump (see game.c) returns 1 so the engine
 *    skips reading the stick for tilt and reaches here to apply the suspension. We
 *    don't want to touch the hydraulics at all -- just freeze them where they are --
 *    so we branch straight to +0xb50 (0x6a2adc), the same "not a hydraulics car, do
 *    nothing" path the function's own gate takes, which cleanly returns without
 *    changing the suspension. In tilt mode we reproduce the clobbered 4 instrs
 *    (ending in `cmp w8,#0x1f4`, whose flags the next instr uses) and rejoin at
 *    +0x4d0 (0x6a2468).
 *
 *  - aim_lr_stub / aim_ud_stub: hooked at CPad::AimWeaponLeftRight+0xa0 (0x49fdcc)
 *    and CPad::AimWeaponUpDown+0x94 (0x4a019c). The car camera reads the right
 *    stick through these; in a hydraulics car they early-return 0 on the
 *    "hydraulics installed" flag (veh+0x4BA bit1), locking the camera. In camera
 *    mode we skip that early-return so the stick reaches the camera. The preceding
 *    `cbz x0,+N` (whose target lands inside our hook) is NOP'd in game.c and its
 *    x0==0 case handled here instead. We then reproduce `str xzr,[sp]` and the
 *    `bl CHID::GetInputType` (by pre-loading x30 with the rejoin address and
 *    tail-branching to GetInputType, so its ret lands there).
 *
 * Written in .s for the same reasons as fov_stub.s (naked is ignored by this GCC;
 * hidden globals keep adrp/:lo12: PIC-valid). The runtime target addresses and the
 * GetInputType pointer are resolved in game.c's patch_game(). x17 is dead on entry
 * (the hook_arm64 trampoline uses it) and used here as scratch; x30 is restored
 * from AimWeapon's own stack frame in its epilogue, so clobbering it is safe.
 */

/* ---- HydraulicControl: freeze the hydraulics in camera mode ----------------- */
.section .text.hyd_freeze_stub, "ax", %progbits
.global hyd_freeze_stub
.type   hyd_freeze_stub, %function
.align  2
hyd_freeze_stub:
    adrp x17, g_hydraulics_camera
    ldr  w17, [x17, #:lo12:g_hydraulics_camera]
    cbz  w17, 1f                  // tilt mode -> run the normal suspension apply
    adrp x17, hyd_freeze_ret      // camera mode -> freeze: jump to the do-nothing
    add  x17, x17, :lo12:hyd_freeze_ret //           return path (0x6a2adc)
    ldr  x17, [x17]
    br   x17
1:
    ldrh w8, [x19, #2696]         // reproduce clobbered instrs (last sets flags for
    str  w28, [sp, #20]           // the b.cc at the rejoin point)
    str  s11, [sp, #12]
    cmp  w8, #0x1f4
    adrp x17, hyd_cont_ret
    add  x17, x17, :lo12:hyd_cont_ret
    ldr  x17, [x17]
    br   x17
.size hyd_freeze_stub, .-hyd_freeze_stub

/* ---- AimWeaponLeftRight: free the camera X in camera mode ------------------- */
.section .text.aim_lr_stub, "ax", %progbits
.global aim_lr_stub
.type   aim_lr_stub, %function
.align  2
aim_lr_stub:
    cbz  x0, 2f                   // vehicle null (the NOP'd cbz path) -> continue
    ldrb w8, [x0, #1210]          // veh+0x4BA flags (reproduce)
    adrp x17, g_hydraulics_camera
    ldr  w17, [x17, #:lo12:g_hydraulics_camera]
    cbnz w17, 2f                  // camera mode -> skip the hydraulics early-return
    tbz  w8, #1, 2f               // no hydraulics installed -> continue
    adrp x17, aim_lr_early        // hydraulics + tilt mode -> early return (0x49fd5c)
    add  x17, x17, :lo12:aim_lr_early
    ldr  x17, [x17]
    br   x17
2:
    str  xzr, [sp]                // reproduce 0x49fdd4
    adrp x17, aim_lr_cont         // x30 = rejoin address 0x49fddc
    add  x17, x17, :lo12:aim_lr_cont
    ldr  x30, [x17]
    adrp x17, chid_getinputtype   // tail-call GetInputType; its ret -> 0x49fddc
    add  x17, x17, :lo12:chid_getinputtype
    ldr  x17, [x17]
    br   x17
.size aim_lr_stub, .-aim_lr_stub

/* ---- AimWeaponUpDown: free the camera Y in camera mode ---------------------- */
.section .text.aim_ud_stub, "ax", %progbits
.global aim_ud_stub
.type   aim_ud_stub, %function
.align  2
aim_ud_stub:
    cbz  x0, 2f                   // vehicle null (the NOP'd cbz path) -> continue
    ldrb w8, [x0, #1210]          // veh+0x4BA flags (reproduce)
    adrp x17, g_hydraulics_camera
    ldr  w17, [x17, #:lo12:g_hydraulics_camera]
    cbnz w17, 2f                  // camera mode -> skip the hydraulics early-return
    tbz  w8, #1, 2f               // no hydraulics installed -> continue
    adrp x17, aim_ud_early        // hydraulics + tilt mode -> early return (0x4a0130)
    add  x17, x17, :lo12:aim_ud_early
    ldr  x17, [x17]
    br   x17
2:
    str  xzr, [sp]                // reproduce 0x4a01a4
    adrp x17, aim_ud_cont         // x30 = rejoin address 0x4a01ac
    add  x17, x17, :lo12:aim_ud_cont
    ldr  x30, [x17]
    adrp x17, chid_getinputtype   // tail-call GetInputType; its ret -> 0x4a01ac
    add  x17, x17, :lo12:chid_getinputtype
    ldr  x17, [x17]
    br   x17
.size aim_ud_stub, .-aim_ud_stub
