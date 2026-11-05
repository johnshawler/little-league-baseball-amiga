/* Linker script, Little League Baseball for the A1200 (2 MB chip RAM, no OS after the bootblock).
 *
 * One flat image linked at PAYLOAD_BASE (0x800) and loaded there by the bootblock.  Stored on the disk: .text .rodata .data and the 256 KB .rom
 * (a zero-filled placeholder patched by tools/build_adf.sh with the PRG+CHR of the .nes file).  .bss, the stack and the display buffers are NOLOAD
 * (.bss is cleared by crt0).  Everything lives in chip RAM.
 *
 *   0x000000-0x0007FF  vectors (0-0xFF) + bootblock relocator (0x200)
 *   0x000800-          .text (translated code, runtime, platform) .rodata .data .rom
 *   (after)            .bss: dispatch tables 8 x 64 KB, PPU/NES state with the 60 KB framebuffer, APU, display shadows, profiler
 *                      .stack, .chipbuf (two 60 KB bitplane buffers)
 */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
PROVIDE(_chip_limit = 0x200000);
PROVIDE(_stack_size = 0x4000);
SECTIONS
{
    . = 0x800;
    _payload_base = .;
    .text : {
        KEEP(*(.text.start))
        _pr0 = .;  *code.o(.text .text.*)                                   /* translated 6502 code */
        _pr1 = .;  *rt_x_s.o(.text .text.*)                                 /* assembler glue: dispatch, helper entry */
        _pr2 = .;  *rt_x.o(.text .text.*)                                   /* C helpers: I/O access, cycle limit, slow path, wait-loop skipping */
        _pr3 = .;  *rt_core.o(.text .text.*)                                /* time model, PPU register access, NMI logic */
        _pr4 = .;  *ppu_ref.o(.text .text.*)                                /* PPU: line renderer, per-dot model */
        _pr5 = .;  *cpu6502.o(.text .text.*) *mapper_mmc1.o(.text .text.*)  /* interpreter (slow path), mapper */
        _pr6 = .;  *apu.o(.text .text.*)                                    /* APU */
        _pr7 = .;  *arender.o(.text .text.*)                                 /* planar renderer (runtime/arender.c) */
        _pr8 = .;  *c2p.o(.text .text.*) *display.o(.text .text.*) *display_ar.o(.text .text.*) *display_ar2.o(.text .text.*) *main.o(.text .text.*) *audio.o(.text .text.*) *platform.o(.text .text.*) *telem.o(.text .text.*) *crt0.o(.text .text.*) *isr.o(.text .text.*) *libc.o(.text .text.*) *tabs.o(.text .text.*)
        _idle0 = .; *(.text.idle) _idle1 = .;                               /* STOP loop of the pacing wait */
        _pr9 = .;  *(.text .text.*)                                         /* libgcc (64-bit division) and the rest */
        _pr10 = .;
        *(.rodata .rodata.*)
    }
    .data : ALIGN(4) { *(.data .data.*) . = ALIGN(4); }
    .rom : ALIGN(4) { rom_start = .; *(.rom) . = ALIGN(4); }
    _load_end = .;
    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }
    .stack (NOLOAD) : ALIGN(4) { . += _stack_size; _stack_top = .; }
    .chipbuf (NOLOAD) : ALIGN(16) { _chip_start = .; *(.chipbuf) . = ALIGN(4); _chip_end = .; }
    _end = .;
    ASSERT(_end <= _chip_limit - 0x1000, "image + buffers do not fit into 2 MB of chip RAM")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
