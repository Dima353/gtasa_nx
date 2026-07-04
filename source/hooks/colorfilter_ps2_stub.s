/* colorfilter_ps2_stub.s -- PS2-style color filter (SkyGFX / Vita skygfx_colorfilter).
 *
 * GTA:SA's mobile build applies a "mobile" post-process color grade in
 * CPostEffects::MobileRender; the PS2 build used a simpler grade driven directly by
 * the two timecycle postfx colours. Vita's ColorFilter replaces the grading matrix
 * diagonal (red.r/green.g/blue.b) with the PS2 formula; off-diagonals stay 0 (the
 * mobile grade only ever touches the diagonal, so they are already 0).
 *
 * arm64 CPostEffects::MobileRender (libGame.so @0x5e2c9c) computes the MOBILE grade
 * and stores its diagonal at +0x614 (0x5e32b0):
 *     stur s20,[x29,#-48]  ; red.r   = s4*s1  (mobile)
 *     stur s19,[x29,#-60]  ; green.g = s3*s0
 *     str  s18,[sp,#72]    ; blue.b  = s5*s2
 *     b.ne 0x5e334c        ; (cmp w8,#1 at +0x610 = darkness-filter flag)
 * The current-frame postfx colours were clamped onto the stack just above:
 *     postfx1 R/G/B/A @ sp+60/61/62/63 ; postfx2 R/G/B/A @ sp+56/57/58/59
 * PS2 formula (a = postfx2.alpha/128):
 *     red.r   = postfx1.R/128 + a*postfx2.R/128
 *     green.g = postfx1.G/128 + a*postfx2.G/128
 *     blue.b  = postfx1.B/128 + a*postfx2.B/128
 *
 * We hook +0x614, recompute the diagonal into s20/s19/s18, reproduce the 3 stores,
 * then reproduce the `b.ne`. The `cmp w8,#1` sits BEFORE the hook and NONE of our
 * instructions touch NZCV (ldrb/ucvtf/fmov/fmul/fadd/fdiv/str leave the condition
 * flags alone), so the darkness-filter branch decision is preserved -- we just
 * re-issue b.ne to 0x5e334c (colorfilter_ret_ne) else fall through to 0x5e32c0
 * (colorfilter_ret_fall). s0-s5 are dead at the hook (mobile intermediates already
 * consumed); w8 (darkness flag) is left untouched; w9/x9 are scratch. Both return
 * targets are hidden globals set in game.c (per fov_stub.s).
 */

.section .text.CPostEffects__MobileRender_ps2filter_stub, "ax", %progbits
.global CPostEffects__MobileRender_ps2filter_stub
.type   CPostEffects__MobileRender_ps2filter_stub, %function
.align  2
CPostEffects__MobileRender_ps2filter_stub:
    mov  w9, #0x43000000         // 128.0f
    fmov s5, w9                  // s5 = 128.0
    ldrb w9, [sp, #59]           // postfx2.alpha
    ucvtf s4, w9
    fdiv s4, s4, s5              // s4 = a = postfx2.alpha/128

    ldrb w9, [sp, #60]           // postfx1.R
    ucvtf s0, w9
    ldrb w9, [sp, #56]           // postfx2.R
    ucvtf s1, w9
    fmul s1, s1, s4              // a*postfx2.R
    fadd s0, s0, s1              // postfx1.R + a*postfx2.R
    fdiv s20, s0, s5             // red.r = (...)/128

    ldrb w9, [sp, #61]           // postfx1.G
    ucvtf s0, w9
    ldrb w9, [sp, #57]           // postfx2.G
    ucvtf s1, w9
    fmul s1, s1, s4
    fadd s0, s0, s1
    fdiv s19, s0, s5             // green.g

    ldrb w9, [sp, #62]           // postfx1.B
    ucvtf s0, w9
    ldrb w9, [sp, #58]           // postfx2.B
    ucvtf s1, w9
    fmul s1, s1, s4
    fadd s0, s0, s1
    fdiv s18, s0, s5             // blue.b

    stur s20, [x29, #-48]        // red.r
    stur s19, [x29, #-60]        // green.g
    str  s18, [sp, #72]          // blue.b

    b.ne 1f                      // NZCV still = cmp w8,#1 (darkness filter)
    adrp x9, colorfilter_ret_fall
    add  x9, x9, :lo12:colorfilter_ret_fall
    ldr  x9, [x9]               // 0x5e32c0
    br   x9
1:
    adrp x9, colorfilter_ret_ne
    add  x9, x9, :lo12:colorfilter_ret_ne
    ldr  x9, [x9]              // 0x5e334c
    br   x9
.size CPostEffects__MobileRender_ps2filter_stub, .-CPostEffects__MobileRender_ps2filter_stub
