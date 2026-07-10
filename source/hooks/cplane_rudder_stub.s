/* cplane_rudder_stub.s -- Hydra/plane yaw (rudder) on L1/R1.
 *
 * On NX the plane rudder is read inline from the left analog stick
 * (CPad::GetSteeringLeftRight) in CPlane::ProcessControlInputs at +0x220 (0x6cc9e8),
 * feeding the rudder accumulator [x19+3016]. That makes the stick both roll AND yaw.
 * The user wants the stick to only roll (tilt) and the yaw to be on L1/R1 (like PSV,
 * which reads the turret buttons for the rudder). We remapped TURRET_LEFT/RIGHT
 * (actions 38/39) to L1/R1, so here we replace the stick read with a turret read:
 * CPlane__rudder_turret_input returns +128 (R1/yaw right), -128 (L1/yaw left) or 0,
 * in the same fixed-point range the code expects (scvtf ...,#7 divides by 128).
 *
 * Hooked at +0x220, replacing the 4 instrs `bl GetSteeringLeftRight; sxth w8,w0;
 * ldr s0,[x19,#3016]; ldr s2,[x21]` and rejoining at +0x230 (0x6cc9f8). x19 (CPlane)
 * and x21 (timestep ptr) are callee-saved so they survive the C call; s0/s2 are set
 * after it. The original `bl` already clobbered x30, so calling C here is safe.
 * cplane_rudder_ret is defined in game.c.
 */

.section .text.CPlane__rudder_turret_stub, "ax", %progbits
.global CPlane__rudder_turret_stub
.type   CPlane__rudder_turret_stub, %function
.align  2
CPlane__rudder_turret_stub:
    bl   CPlane__rudder_turret_input   // w0 = +128 (R1) / -128 (L1) / 0
    sxth w8, w0                         // reproduce +0x224 (w8 = rudder input, used
                                        //   by the scvtf at the rejoin -- keep it!)
    ldr  s0, [x19, #3016]               // reproduce +0x228 (rudder accumulator)
    ldr  s2, [x21]                      // reproduce +0x22c (timestep)
    adrp x17, cplane_rudder_ret         // x17 (not x8!) -- x8/w8 holds the rudder
    add  x17, x17, :lo12:cplane_rudder_ret //  input the rejoin's scvtf reads
    ldr  x17, [x17]                     // runtime addr of ProcessControlInputs+0x230
    br   x17
.size CPlane__rudder_turret_stub, .-CPlane__rudder_turret_stub
