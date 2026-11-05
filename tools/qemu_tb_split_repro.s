/* qemu_tb_split_repro.s - qemu-m68k 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18) evaluates a conditional branch wrongly when a translation block ends between the BTST and the
 * Bcc (qemu stops a block some 30 bytes before a 4 KB page end).  The idiom is the translator's test of a canonical 6502 flag:
 *     move.b #0,d0 ; move.w ccr,d7 ; and.w #12,d7 ; move.b (a5,d7.w),(a2) ; move.b d0,(184,a0) ; btst #1,(a2) ; bne <taken>
 * Build: m68k-linux-gnu-as -m68020 --register-prefix-optional tools/qemu_tb_split_repro.s -o r.o && m68k-linux-gnu-ld -o r r.o
 * Run `qemu-m68k -cpu m68020 ./r; echo $?` : 232 = correct (branch taken in all 1000 iterations); 89 = the branch was not taken.  The result depends on where the sequence
 * sits relative to the page end: change the `.space 4096-0x50` below (0x3e and 0x40 fail with qemu 8.2.2, 0x50 passes).  `qemu-m68k -singlestep` gives 232 for all offsets.
 * Real 68020 / UAE semantics: BTST changes only Z, the branch is always taken here. */
	.text
	.globl _start
_start:
	lea ram,%a0
	lea ctxb,%a2
	lea nztab,%a5
	moveq #0,%d3
	move.l #100000,%d5
	moveq #0,%d6
loop:
	jmp B1_94A4
back:
	addq.l #1,%d6
	cmp.l #1000,%d6
	bne.w loop
	move.l %d5,%d1
	moveq #1,%d0
	/* exit code = low byte of (100000 - d5)/8 */
	move.l #100000,%d2
	sub.l %d5,%d2
	lsr.l #3,%d2
	move.l %d2,%d1
	trap #0
	.balign 4096
	.space 4096-0x50
B1_94A4:
	sub.l #13,d5
	bmi .L571
	move.b #0,d0
	move.w ccr,d7
	and.w #12,d7
	move.b (a5,d7.w),(0,a2)
	move.b d0,(184,a0)
	btst #1,(0,a2)
	bne .L572
	move.b #148,(0x100,a0,d3.w)
	subq.b #1,d3
	move.b #172,(0x100,a0,d3.w)
	subq.b #1,d3
	jmp B1_955C
.L571:
	add.l #13,d5
	move.w #0x94A4,d7
	jmp slow
.L572:
	add.l #5,d5
	jmp back
B1_955C:
	jmp back
slow:
	jmp back
	.data
ram: .space 512
ctxb: .space 16
nztab: .byte 0,0,0,0,2,0,0,0,0,0,0,0,0x80,0,0,0
