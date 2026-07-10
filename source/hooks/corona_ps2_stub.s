/* corona_ps2_stub.s -- PS2-style corona rotation.
 *
 * Ported from JPatch's "PS2CoronaRotation" (patches_sa64.inl, HOOKBL at the
 * RenderOneXLUSprite_Rotate_Aspect call site inside the corona renderer). On PC/PS2
 * the light coronas spin; the mobile build passes rotation = 0 so they sit static.
 *
 * The single corona call site is in CCoronas::Render (libGame.so @0x5ce048), the
 * `bl CSprite::RenderOneXLUSprite_Rotate_Aspect` at +0x418 (0x5ce460). Its float
 * args: s5 = rz (7th, recipZ-derived size), s6 = rotation (8th). JPatch re-invokes
 * with rz' = rz*0.05, rotation' = rz. In this build the stock code sets
 * s6 = 0 and s5 = raw * 20.0 right before the call, so JPatch's rz == raw*20 and:
 *     rz'       = rz * 0.05 = raw*20*0.05 = raw        -> leave s5 = raw (skip *20)
 *     rotation' = rz        = raw*20                   -> s6 = raw*20
 *
 * We hook CCoronas::Render+0x404 (0x5ce44c), replacing the 4 clobbered instructions
 *   movi d6,#0            ; s6 = 0        -> fmul s6,s5,s0  (rotation = raw*20)
 *   fmul s5,s5,s0         ; s5 = raw*20   -> dropped        (s5 stays = raw = rz')
 *   ldp  s0,s1,[x29,#-96] ; pos.x,pos.y   -> reproduced
 *   cmp  w10,#0xff                        -> reproduced
 * and rejoin at +0x414 (0x5ce45c, `csel w2,w10,w28,cc`). s0 still holds 20.0 (set at
 * +0x400) when we compute s6, so the multiply is done before s0 is reloaded from the
 * stack. No inbound branches land in the clobbered range. corona_ps2_ret is defined
 * in game.c (hidden global -> adrp/add stay PIC-valid, per fov_stub.s).
 */

.section .text.CCoronas__Render_ps2corona_stub, "ax", %progbits
.global CCoronas__Render_ps2corona_stub
.type   CCoronas__Render_ps2corona_stub, %function
.align  2
CCoronas__Render_ps2corona_stub:
    fmul s6, s5, s0              // rotation = raw * 20.0  (s0 == 20.0 here)
                                 // s5 left = raw  ==  rz * 0.05
    ldp  s0, s1, [x29, #-96]     // pos.x, pos.y
    cmp  w10, #0xff
    adrp x17, corona_ps2_ret
    add  x17, x17, :lo12:corona_ps2_ret
    ldr  x17, [x17]             // runtime address of CCoronas::Render+0x414
    br   x17
.size CCoronas__Render_ps2corona_stub, .-CCoronas__Render_ps2corona_stub
