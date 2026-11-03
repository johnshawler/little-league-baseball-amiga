/* unit-test glue for the translated code: runs one translated routine with the registers/flags/RAM given in an XCtx and returns the result.
   Mirrors the register conventions of translator/x65.py. */
	.text
	.globl xl_call, F_FFF0, rt_disp, rt_rd, rt_wr, rt_slow, rt_unk, rt_unk_ent
/* void xl_call(XCtx *c)  c layout: 0 nzc,1 vflag,2 misc,4 adj(w), 8 d0..d5 (6 longs: 8..31), 32 entry, 36 saved sp, 40 ram, 44 win, 48 tabwin, 52 tabfix, 56 nztab, 60 fixbase */
xl_call:
	move.l 4(sp),a0
	movem.l d2-d7/a2-a6,-(sp)
	move.l a0,a2
	move.l sp,36(a2)
	move.l 40(a2),a0
	move.l 44(a2),a1
	move.l 48(a2),a3
	move.l 52(a2),a4
	move.l 56(a2),a5
	move.l 60(a2),a6
	movem.l 8(a2),d0-d5
	move.l 32(a2),a0   /* temporarily entry */
	move.l 40(a2),a0
	move.l 32(a2),-(sp)
	rts                 /* jump to entry (pushed as return address) */
F_FFF0:
	movem.l d0-d5,8(a2)
	move.l 36(a2),sp
	movem.l (sp)+,d2-d7/a2-a6
	rts
rt_disp:
	and.l #0xFFFF,d7
	cmp.w #0xC000,d7
	bcc.s 1f
	jmp ([a3,d7.l*4])
1:	jmp ([a4,d7.l*4])
rt_slow:
rt_unk:
rt_unk_ent:
	illegal
/* helpers: d7.w = address, d6.b = value (wr) / result (rd) */
rt_rd:
	movem.l d0-d1/a0-a1,-(sp)
	moveq #0,d0
	move.w d7,d0
	move.l d0,-(sp)
	jsr xl_rd_c
	addq.l #4,sp
	move.b d0,d6
	movem.l (sp)+,d0-d1/a0-a1
	rts
rt_wr:
	movem.l d0-d1/a0-a1,-(sp)
	moveq #0,d0
	move.w d7,d0
	moveq #0,d1
	move.b d6,d1
	move.l d1,-(sp)
	move.l d0,-(sp)
	jsr xl_wr_c
	addq.l #8,sp
	movem.l (sp)+,d0-d1/a0-a1
	rts
