/* rt_x.s - assembler glue for the translated code (see runtime/rt_x.h for the context layout) */
	.text
	.globl xrt_enter, rt_disp, rt_rd, rt_wr, rt_slow, rt_unk, rt_unk_ent
	.extern xrt_ctx, xrt_rd_c, xrt_wr_c, xrt_slow_c
/* void xrt_enter(void): load the translated machine state from xrt_ctx and run (d7 = start address already in ctx->d? : we start via the slow path) */
xrt_enter:
	movem.l d2-d7/a2-a6,-(sp)
	lea xrt_ctx,a2
	move.l sp,36(a2)
	moveq #0,d7
	moveq #-1,d0           /* d0 = -1: ask xrt_slow_c to initialise from the interpreter state */
	bra.s xrt_slow_entry
xrt_exit:
	lea xrt_ctx,a2
	move.l 36(a2),sp
	movem.l (sp)+,d2-d7/a2-a6
	rts
/* dispatch: d7.w = 6502 address */
rt_disp:
	and.l #0xFFFF,d7
	cmp.w #0xC000,d7
	bcc.s 1f
	jmp ([a3,d7.l*4])
1:	jmp ([a4,d7.l*4])
rt_unk_ent:
rt_unk:
	/* d7.w = pc, flags canonical, d5 = limit-now */
rt_slow_entry_common:
	movem.l d0-d5,8(a2)
	moveq #0,d0
	move.w d7,d0
xrt_slow_entry:
	move.l d0,-(sp)
	jsr xrt_slow_c
	addq.l #4,sp
	tst.l d0
	bmi xrt_exit
	lea xrt_ctx,a2
	move.l d0,d7
	movem.l 8(a2),d0-d5
	move.l 40(a2),a0
	move.l 44(a2),a1
	move.l 48(a2),a3
	move.l 52(a2),a4
	move.l 56(a2),a5
	move.l 60(a2),a6
	bra rt_disp
rt_slow:
	movem.l d0-d5,8(a2)
	moveq #0,d0
	move.w d7,d0
	bra.s xrt_slow_entry
/* helpers: d7.w = 6502 address, ctx->adj = access cycle adjustment, d6.b = value (wr) / result (rd); d5 = budget */
rt_rd:
	movem.l d0-d1/a0-a1,-(sp)
	moveq #0,d0
	move.w d7,d0
	move.w 4(a2),d1
	ext.l d1
	move.l d1,-(sp)
	move.l d5,-(sp)
	move.l d0,-(sp)
	jsr xrt_rd_c
	lea 12(sp),sp
	move.b d0,d6
	movem.l (sp)+,d0-d1/a0-a1
	rts
rt_wr:
	movem.l d0-d1/a0-a1,-(sp)
	moveq #0,d0
	move.w d7,d0
	moveq #0,d1
	move.b d6,d1
	move.w 4(a2),d6
	ext.l d6
	move.l d6,-(sp)
	move.l d5,-(sp)
	move.l d1,-(sp)
	move.l d0,-(sp)
	jsr xrt_wr_c
	lea 16(sp),sp
	move.l d0,d6               /* d6 = change of the cycle limit */
	movem.l (sp)+,d0-d1/a0-a1
	add.l d6,d5
	move.l 44(a2),a1           /* the mapper may have changed the window */
	move.l 48(a2),a3
	rts

/* debug: block entry hook (translator option XL_DEBUG_BLOCKS): d7.w = pc, ctx->adj = block cycles, d5 = budget after charging the block */
	.globl rt_dbg_blk
	.extern xrt_dbg_c
rt_dbg_blk:
	movem.l d0-d1/a0-a1,-(sp)
	moveq #0,d0
	move.w d7,d0
	move.w 4(a2),d1
	ext.l d1
	move.l d1,-(sp)
	move.l d5,-(sp)
	move.l d0,-(sp)
	jsr xrt_dbg_c
	lea 12(sp),sp
	movem.l (sp)+,d0-d1/a0-a1
	rts

/* skip: ctx->adj = kind; d5 = budget.  Subtracts the cycles of the polls that can be skipped from d5. */
	.globl rt_skip
	.extern xrt_skip_c
rt_skip:
	movem.l d0-d1/a0-a1,-(sp)
	move.w 4(a2),d1
	ext.l d1
	move.l d5,-(sp)
	move.l d1,-(sp)
	jsr xrt_skip_c
	addq.l #8,sp
	move.l d0,d6
	movem.l (sp)+,d0-d1/a0-a1
	sub.l d6,d5
	rts
