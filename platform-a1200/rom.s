| rom.s - 256 KB placeholder for the cartridge image (PRG 128 KB, then CHR 128 KB), patched into the payload by tools/build_adf.sh (the section start is the linker symbol rom_start).
| The ROM is never part of the repository.
	.section .rom,"aw"
rom_data:
	.space 262144
