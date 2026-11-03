| c2p.s - Little League Baseball A1200: chunky -> 8 interleaved bitplanes, hand-written 68020 kernels used by display.c.
| The 16-pixel kernel (five merge steps, verified against the bit-by-bit transpose in the Zoo Keeper port) is unchanged.
|   c2p_pair(src, dst)             16 chunky bytes (4-byte aligned) -> 8 plane words at dst + 32 * p      (test hook, same code as the frame loop)
|   c2p_frame(src, dst, shadow)    256x240 chunky frame -> interleaved planes (line y of plane p at dst + y*256 + p*32).  `shadow` holds the chunky
|                                  frame that `dst` currently shows: every aligned pair of groups (16 pixels) that equals the shadow is skipped;
|                                  converted pairs are copied to the shadow.  Returns the number of converted pairs.
|   c2p_frame_all(src, dst)        the same without comparison (whole frame)
| C ABI: arguments on the stack, d2-d7 / a2-a6 preserved.
	.ifdef P5
	.equ STRIDE,160
	.else
	.equ STRIDE,256
	.endif
	.text
	.even
	.globl c2p_pair, c2p_frame, c2p_uni_words
| a0 = src, a1 = dst (preserved), clobbers d0-d4 a3
.Lpair:
	move.l (%a0),%d0
	move.l %d0,%d1
	rol.l #8,%d1
	cmp.l %d0,%d1				| all four bytes of the first long equal?
	jbne .Lkern
	cmp.l 4(%a0),%d0
	jbne .Lkern
	cmp.l 8(%a0),%d0
	jbne .Lkern
	cmp.l 12(%a0),%d0
	jbne .Lkern
	and.w #255,%d0				| 16 pixels of one colour: each plane word is all ones or all zeros
	lsl.w #4,%d0
	lea c2p_uni_words,%a3
	lea 0(%a3,%d0.w),%a3
	move.w (%a3)+,(%a1)
	move.w (%a3)+,32(%a1)
	move.w (%a3)+,64(%a1)
	move.w (%a3)+,96(%a1)
	move.w (%a3)+,128(%a1)
	.ifndef P5
	move.w (%a3)+,160(%a1)
	move.w (%a3)+,192(%a1)
	move.w (%a3)+,224(%a1)
	.endif
	rts
.Lkern:
	move.l 12(%a0),%d0
	move.l 8(%a0),%d1
	move.l 4(%a0),%d2
	move.l (%a0),%d3
	move.l %d0,%d4
	lsr.l #8,%d4
	eor.l %d2,%d4
	and.l #0x00FF00FF,%d4
	eor.l %d4,%d2
	lsl.l #8,%d4
	eor.l %d4,%d0
	move.l %d1,%d4
	lsr.l #8,%d4
	eor.l %d3,%d4
	and.l #0x00FF00FF,%d4
	eor.l %d4,%d3
	lsl.l #8,%d4
	eor.l %d4,%d1
	move.l %d0,%d4
	lsr.l #4,%d4
	eor.l %d1,%d4
	and.l #0x0F0F0F0F,%d4
	eor.l %d4,%d1
	lsl.l #4,%d4
	eor.l %d4,%d0
	move.l %d2,%d4
	lsr.l #4,%d4
	eor.l %d3,%d4
	and.l #0x0F0F0F0F,%d4
	eor.l %d4,%d3
	lsl.l #4,%d4
	eor.l %d4,%d2
	swap %d0
	move.w %d0,%d4
	move.w %d1,%d0
	swap %d0
	move.w %d4,%d1
	swap %d2
	move.w %d2,%d4
	move.w %d3,%d2
	swap %d2
	move.w %d4,%d3
	move.l %d0,%d4
	lsr.l #2,%d4
	eor.l %d1,%d4
	and.l #0x33333333,%d4
	eor.l %d4,%d1
	lsl.l #2,%d4
	eor.l %d4,%d0
	move.l %d2,%d4
	lsr.l #2,%d4
	eor.l %d3,%d4
	and.l #0x33333333,%d4
	eor.l %d4,%d3
	lsl.l #2,%d4
	eor.l %d4,%d2
	move.l %d0,%d4
	lsr.l #1,%d4
	eor.l %d2,%d4
	and.l #0x55555555,%d4
	eor.l %d4,%d2
	lsl.l #1,%d4
	eor.l %d4,%d0
	move.l %d1,%d4
	lsr.l #1,%d4
	eor.l %d3,%d4
	and.l #0x55555555,%d4
	eor.l %d4,%d3
	lsl.l #1,%d4
	eor.l %d4,%d1
	move.w %d0,0(%a1)
	swap %d0
	move.w %d0,128(%a1)
	move.w %d1,64(%a1)
	move.w %d2,32(%a1)
	move.w %d3,96(%a1)
	.ifndef P5
	swap %d1
	move.w %d1,192(%a1)
	swap %d2
	move.w %d2,160(%a1)
	swap %d3
	move.w %d3,224(%a1)
	.endif
	rts

c2p_pair:
	movem.l %d2-%d4/%a3,-(%sp)
	move.l 20(%sp),%a0
	move.l 24(%sp),%a1
	jbsr .Lpair
	movem.l (%sp)+,%d2-%d4/%a3
	rts

| long c2p_frame(src, dst, shadow, rowflags)   rows whose flag byte is 0 are skipped, the flag of a processed row is cleared
c2p_frame:
	movem.l %d2-%d7/%a2-%a4,-(%sp)
	move.l 40(%sp),%a0			| src
	move.l 44(%sp),%a1			| dst
	move.l 48(%sp),%a2			| shadow
	move.l 52(%sp),%a4			| row flags
	moveq #0,%d5				| converted pairs
	move.w #239,%d7				| rows
.Lrow:
	tst.b (%a4)+
	jbne .Ldo
	lea 256(%a0),%a0
	lea 256(%a2),%a2
	lea STRIDE(%a1),%a1
	dbra %d7,.Lrow
	jbra .Lfin
.Ldo:
	clr.b -1(%a4)
	moveq #15,%d6				| pairs per row
.Lp:
	move.l (%a0),%d0
	cmp.l (%a2),%d0
	jbne .Lconv
	move.l 4(%a0),%d0
	cmp.l 4(%a2),%d0
	jbne .Lconv
	move.l 8(%a0),%d0
	cmp.l 8(%a2),%d0
	jbne .Lconv
	move.l 12(%a0),%d0
	cmp.l 12(%a2),%d0
	jbne .Lconv
.Lnext:
	lea 16(%a0),%a0
	lea 16(%a2),%a2
	addq.l #2,%a1
	dbra %d6,.Lp
	lea STRIDE-32(%a1),%a1			| next line: stride 256, 32 bytes advanced
	dbra %d7,.Lrow
.Lfin:
	move.l %d5,%d0
	movem.l (%sp)+,%d2-%d7/%a2-%a4
	rts
.Lconv:
	move.l (%a0),(%a2)
	move.l 4(%a0),4(%a2)
	move.l 8(%a0),8(%a2)
	move.l 12(%a0),12(%a2)
	addq.l #1,%d5
	jbsr .Lpair
	jbra .Lnext

| long c2p_frame_all(src, dst): same conversion, no comparison
	.globl c2p_frame_all
c2p_frame_all:
	movem.l %d2-%d7/%a2-%a3,-(%sp)
	move.l 36(%sp),%a0
	move.l 40(%sp),%a1
	move.w #239,%d7
.Lrow2:
	moveq #15,%d6
.Lp2:
	jbsr .Lpair
	lea 16(%a0),%a0
	addq.l #2,%a1
	dbra %d6,.Lp2
	lea STRIDE-32(%a1),%a1
	dbra %d7,.Lrow2
	movem.l (%sp)+,%d2-%d7/%a2-%a3
	rts
