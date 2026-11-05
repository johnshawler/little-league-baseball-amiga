| loader.s - AmigaDOS loader `LittleLeague` for the hard disk image (tools/build_hdf.sh).  Position independent, one CODE hunk (tools/mkhunk.py).
|
| Started from S:Startup-Sequence by AmigaDOS (user mode, KS 2.0+; tested on 3.1), same job as boot.s for the floppy:
| 1. dos.library: Open("PROGDIR:LittleLeague.dat"), size via Seek, AllocMem(size, MEMF_CHIP|MEMF_CLEAR) staging buffer, Read, Close.
|    The payload is the flat image linked at 0x800 (the same llb.bin as in the ADF).  It needs no fast RAM: the staging buffer is chip RAM.
| 2. Supervisor(): mask all interrupts, VBR := 0, wait for the Blitter, all DMA off, clear + disable the caches, copy the relocator to 0x200 and run it
|    from there: it moves the staging buffer to 0x800 (memmove, ascending or descending) and jumps to the entry.  The OS is never used again.
| Errors (before the take-over) are printed to the CLI output and returned as RETURN_FAIL (20): 68040/68060 (the cache handling is the 68020/68030 one),
| payload not found / unreadable, not enough contiguous chip RAM.
    .ifdef  FASTRAM
    .equ    PAYLOAD_BASE, 0x200000      | FASTRAM build: payload linked at the start of the 8 MB fast RAM
    .else
    .equ    PAYLOAD_BASE, 0x800
    .endif
    .equ    RELOC_ADDR,   0x200
    .text
start:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    moveq   #0,%d7                  | d7 = file handle (0 = none)
    moveq   #0,%d5                  | d5 = staging buffer (0 = none)
    moveq   #0,%d6                  | d6 = rounded length
    moveq   #0,%d4                  | d4 = file length
    move.l  4.w,%a3                 | a3 = ExecBase
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    move.l  %a3,%a6
    jsr     -552(%a6)               | OpenLibrary("dos.library", 0)
    move.l  %d0,%a4                 | a4 = DOSBase
    tst.l   %d0
    beq     rc20
    move.w  296(%a3),%d0            | ExecBase->AttnFlags: bit 3 = 68040, bit 7 = 68060
    lea     m_cpu(%pc),%a2
    btst    #3,%d0
    bne     fail
    btst    #7,%d0
    bne     fail
    .ifdef  FASTRAM
    lea     m_fast(%pc),%a2         | fast RAM covering 0x200000-0x9FFFFF: scan ExecBase->MemList for MEMF_FAST with mh_Lower <= 0x200000 and mh_Upper >= 0xA00000
    move.l  322(%a3),%a0
1:  move.l  (%a0),%d0
    beq     fail
    btst    #2,15(%a0)
    beq.s   2f
    cmp.l   #0x200100,20(%a0)
    bhi.s   2f
    cmp.l   #0xA00000,24(%a0)
    bcc.s   3f
2:  move.l  %d0,%a0
    bra.s   1b
3:
    .endif
    move.l  %a4,%a6
    lea     fname(%pc),%a0
    move.l  %a0,%d1
    move.l  #1005,%d2               | MODE_OLDFILE
    jsr     -30(%a6)                | Open
    move.l  %d0,%d7
    lea     m_open(%pc),%a2
    beq     fail
    move.l  %d7,%d1                 | Seek(fh, 0, OFFSET_END)
    moveq   #0,%d2
    moveq   #1,%d3
    jsr     -66(%a6)
    move.l  %d7,%d1                 | Seek(fh, 0, OFFSET_BEGINNING) returns the previous position = file length
    moveq   #0,%d2
    moveq   #-1,%d3
    jsr     -66(%a6)
    move.l  %d0,%d4
    lea     m_read(%pc),%a2
    ble     fail
    move.l  %d4,%d6
    addq.l  #3,%d6
    moveq   #-4,%d0
    and.l   %d0,%d6                 | d6 = length rounded up to a multiple of 4
    move.l  %d6,%d0
    move.l  #0x10002,%d1            | MEMF_CHIP | MEMF_CLEAR
    move.l  %a3,%a6
    jsr     -198(%a6)               | AllocMem
    move.l  %d0,%d5
    lea     m_mem(%pc),%a2
    beq     fail
    move.l  %a4,%a6
    move.l  %d7,%d1                 | Read(fh, buffer, length)
    move.l  %d5,%d2
    move.l  %d4,%d3
    jsr     -42(%a6)
    cmp.l   %d4,%d0
    lea     m_read(%pc),%a2
    bne     fail
    move.l  %d7,%d1
    jsr     -36(%a6)                | Close
    move.l  %a4,%a1
    move.l  %a3,%a6
    jsr     -414(%a6)               | CloseLibrary(dos)
    lea     super(%pc),%a5
    jsr     -30(%a6)                | Supervisor(a5) - never returns

fail:                               | a2 = message; print it, release everything, RETURN_FAIL
    move.l  %a4,%a6
    jsr     -60(%a6)                | Output()
    move.l  %d0,%d1
    beq.s   2f
    move.l  %a2,%d2
    moveq   #0,%d3
1:  tst.b   (%a2)+
    beq.s   3f
    addq.l  #1,%d3
    bra.s   1b
3:  jsr     -48(%a6)                | Write(out, msg, len)
2:  tst.l   %d7
    beq.s   4f
    move.l  %d7,%d1
    jsr     -36(%a6)                | Close
4:  tst.l   %d5
    beq.s   5f
    move.l  %d5,%a1
    move.l  %d6,%d0
    move.l  %a3,%a6
    jsr     -210(%a6)               | FreeMem
5:  move.l  %a4,%a1
    move.l  %a3,%a6
    jsr     -414(%a6)               | CloseLibrary
rc20:
    moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

super:                              | supervisor mode, d5 = source, d6 = length
    move.w  #0x2700,%sr
    lea     0x400.w,%sp
    move.w  #0x7FFF,0xDFF09A        | INTENA off
    move.w  #0x7FFF,0xDFF09C        | INTREQ clear
    moveq   #0,%d0
    movec   %d0,%vbr                | vectors at chip address 0 (crt0 installs them)
    move.b  0xDFF002,%d0            | DMACONR: dummy read (BBUSY quirk)
2:  btst    #6,0xDFF002             | wait for Blitter idle (BBUSY, bit 14)
    bne.s   2b
    move.w  #0x7FFF,0xDFF096        | all DMA off
    move.w  #0x7FFF,0xDFF09C
    move.l  #0x0808,%d0             | CACR: clear I-cache (CI) and D-cache (CD, 68030), both disabled
    movec   %d0,%cacr
    lea     reloc(%pc),%a0          | install relocator at RELOC_ADDR
    lea     RELOC_ADDR.w,%a1
    moveq   #(reloc_end-reloc)/2-1,%d0
3:  move.w  (%a0)+,(%a1)+
    dbra    %d0,3b
    move.l  %d5,%a0                 | src = staging
    lea     PAYLOAD_BASE,%a1        | dst
    move.l  %d6,%d0
    lsr.l   #2,%d0                  | longwords
    move.l  %d6,%d7
    jmp     RELOC_ADDR.w

reloc:                              | position independent (same as boot.s)
    cmp.l   %a0,%a1
    bhi.s   2f                      | dst > src: copy descending
1:  move.l  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   1b
    jmp     PAYLOAD_BASE
2:  lea     0(%a0,%d7.l),%a0
    lea     0(%a1,%d7.l),%a1
3:  move.l  -(%a0),-(%a1)
    subq.l  #1,%d0
    bne.s   3b
    jmp     PAYLOAD_BASE
reloc_end:

dosname: .asciz "dos.library"
fname:   .asciz "PROGDIR:LittleLeague.dat"
m_cpu:   .asciz "LittleLeague: 68040/68060 not supported\n"
m_open:  .asciz "LittleLeague: cannot open PROGDIR:LittleLeague.dat\n"
m_read:  .asciz "LittleLeague: cannot read LittleLeague.dat\n"
m_mem:   .asciz "LittleLeague: not enough chip RAM\n"
    .ifdef  FASTRAM
m_fast:  .asciz "LittleLeague: needs 8 MB FAST RAM at $200000 (none found)\n"
    .endif
    .even
