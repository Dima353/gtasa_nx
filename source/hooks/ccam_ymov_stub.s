/* ccam_ymov_stub.s -- mid-function stub for the Hydra camera fix (part 2).
 *
 * Hooked into CCam::Process_FollowCar_SA (libGame.so). After the FindPlayerVehicle
 * NOPs freed the right analog stick for the camera, the stick moved the camera on
 * BOTH axes. In the Hydra we want the horizontal axis to keep steering the camera
 * while the vertical axis is freed (it drives the thruster nozzles instead). Here
 * the camera's vertical rotation delta ("yMovement", ARMv7 s28) lives in s15 and
 * the horizontal delta ("xMovement", ARMv7 s21) in s8. We zero s15 when the
 * followed vehicle is a Hydra (model 520), leaving s8 untouched.
 *
 * Replaces the 4 instructions at Process_FollowCar_SA+0xF60 (bl GetInputType;
 * cmp #2; b.ne +0xF74; ldrh [ped+0x110]) and branches to +0xF74 (0x454ff0), which
 * is exactly where the GetInputType!=2 path already goes -- so we simply skip the
 * touch-input (GetInputType==2) special case, matching the Vita fix.
 *
 * x21 = the followed vehicle (modelIndex at +0x32) throughout the function. x8/w0
 * are dead across the replaced instructions (x8 is reloaded at +0xF7C, w0 was the
 * bl result), so both are safe scratch. Written in a .s file for the same reasons
 * as fov_stub.s (naked is ignored by this GCC; hidden global keeps adrp/add PIC).
 * ccam_ymov_ret is defined in game.c.
 */

.section .text.CCam__FollowCar_ymov_stub, "ax", %progbits
.global CCam__FollowCar_ymov_stub
.type   CCam__FollowCar_ymov_stub, %function
.align  2
CCam__FollowCar_ymov_stub:
    ldrh w8, [x21, #0x32]         // followed vehicle modelIndex
    cmp  w8, #0x208               // Hydra == 520?
    b.ne 1f
    movi d15, #0x0                // s15 = 0 -> zero the vertical camera delta
1:
    adrp x8, ccam_ymov_ret
    add  x8, x8, :lo12:ccam_ymov_ret
    ldr  x8, [x8]                 // runtime address of Process_FollowCar_SA+0xF74
    br   x8
.size CCam__FollowCar_ymov_stub, .-CCam__FollowCar_ymov_stub
