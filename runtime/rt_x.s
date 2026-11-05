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
.ifdef XRT_STATS
	addq.l #1,xrt_st_unk
.endif
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
.ifdef XRT_STATS
	addq.l #1,xrt_st_slow
.endif
	movem.l d0-d5,8(a2)
	moveq #0,d0
	move.w d7,d0
	bra.s xrt_slow_entry
/* ---------------------------------------------------------------------------------------------------------------------------------------
 * I/O helper interface (called by the translated code with  move.w #adj,(4,a2) ; jsr rt_rd / rt_wr ):
 *   d7.w = 6502 address, ctx->adj = adjustment of the access cycle (access cycle = limit - d5 + adj), d5 = budget (limit - now),
 *   d6.b = value to write (rt_wr) / result (rt_rd, on return), (3,a2) = open bus value for the next read (high byte of the operand address).
 *   Preserved: d0-d5, a0-a6.  Clobbered: d6, d7.  rt_wr may change the budget d5 and the window registers a1/a3 (mapper write).
 * Assembler fast paths (no C call): MMC1 writes that only shift the serial register (4 of 5 writes), reads of $4016/$4017.
 * Everything else goes through xrt_rd_c / xrt_wr_c (C).  A translator can call the fast paths directly for static addresses: rt_wr_mmc1 (d6=value,
 * address >= $8000), rt_rd_joy0 / rt_rd_joy1 (result in d6, needs (3,a2)); they fall back to the general path when they cannot handle the access.
 * Host verification builds (-DXRT_TRACE) emit the trace records of the accesses the fast paths handle.
 * Layout constants (offsets in Nes / Rt) are checked by static asserts in rt_x.c.
 * --------------------------------------------------------------------------------------------------------------------------------------- */
	.globl rt_wr_mmc1, rt_rd_joy0, rt_rd_joy1
	.extern xrt_trace_c, nes, rt
.macro TRACE_CALL type
.ifdef XRT_TRACE
	movem.l d0-d1/a0-a1,-(sp)
	move.w 4(a2),d1
	ext.l d1
	move.l d1,-(sp)
	move.l d5,-(sp)
	moveq #0,d1
	move.b d6,d1
	move.l d1,-(sp)
	moveq #0,d1
	move.w d7,d1
	move.l d1,-(sp)
	move.l #\type,-(sp)
	jsr xrt_trace_c
	lea 20(sp),sp
	movem.l (sp)+,d0-d1/a0-a1
.endif
.endm
rt_rd:
	tst.w d7
	bmi.s 3f                   /* $8000-$FFFF: ROM through the windows (a1 = $8000.. window, a6 = fixed bank), no side effects */
	cmp.w #0x2000,d7
	bcc.s 2f
	and.l #0x7FF,d7            /* RAM (and its mirrors) */
	move.b (0,a0,d7.l),d6
	move.b d6,rt+194           /* open bus */
	rts
3:	and.l #0xFFFF,d7
	cmp.w #0xC000,d7
	bcc.s 4f
	move.b (-0x8000,a1,d7.l),d6
	move.b d6,rt+194
	rts
4:	move.b (-0x8000,a6,d7.l),d6
	move.b d6,rt+194
	rts
2:	cmp.w #0x4016,d7
	beq rt_rd_joy0
	cmp.w #0x4017,d7
	beq rt_rd_joy1
rt_rd_gen:
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
rt_rd_joy0:
	tst.b nes+38
	beq.s 1f
	move.b nes+32,nes+36
1:	move.b nes+36,d6
	move.b d6,d7
	lsr.b #1,d7
	or.b #0x80,d7
	move.b d7,nes+36
	and.b #1,d6
	move.b 3(a2),d7
	and.b #0xE0,d7
	or.b d7,d6
	move.b d6,rt+194
	move.w #0x4016,d7
	TRACE_CALL 82
	rts
rt_rd_joy1:
	tst.b nes+38
	beq.s 1f
	move.b nes+32+1,nes+36+1
1:	move.b nes+36+1,d6
	move.b d6,d7
	lsr.b #1,d7
	or.b #0x80,d7
	move.b d7,nes+36+1
	and.b #1,d6
	move.b 3(a2),d7
	and.b #0xE0,d7
	or.b d7,d6
	move.b d6,rt+194
	move.w #0x4017,d7
	TRACE_CALL 82
	rts
rt_wr:
	cmp.w #0x2000,d7
	bcc.s 1f
	and.l #0x7FF,d7            /* RAM */
	move.b d6,(0,a0,d7.l)
	move.b d6,rt+194           /* open bus */
	rts
1:	cmp.w #0x8000,d7
	bcs.s rt_wr_gen
rt_wr_mmc1:
.ifdef XRT_CHECK
	bra rt_wr_gen              /* host self-check (PPU_CHECK) mirrors mapper writes to the reference instance in C */
.endif
	tst.b d6
	bmi.s rt_wr_gen            /* bit 7: reset of the shift register */
	cmp.b #4,nes+9
	beq.s rt_wr_gen            /* the fifth write loads a register: banks, mirroring, PPU synchronisation */
	TRACE_CALL 87
	addq.b #1,nes+9
	and.b #1,d6
	lsl.b #4,d6
	move.b nes+8,d7
	lsr.b #1,d7
	or.b d6,d7
	move.b d7,nes+8
	rts
rt_wr_gen:
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

.ifdef XRT_STATS
	.data
	.globl xrt_st_unk, xrt_st_slow
xrt_st_unk:	.long 0
xrt_st_slow:	.long 0
	.text
.endif
