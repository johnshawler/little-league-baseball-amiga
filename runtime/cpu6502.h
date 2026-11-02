/* cpu6502.h - cycle-exact (bus-cycle exact) Ricoh 2A03 (6502 without decimal mode) core, documented opcodes only.
 * Target independent.  Every memory access goes through bus_rd/bus_wr and costs exactly one CPU cycle; the bus functions advance time. */
#ifndef CPU6502_H
#define CPU6502_H
#include <stdint.h>
typedef struct { uint16_t pc; uint8_t a, x, y, s, p; } Cpu6502;   /* p: bit5 always 1; bit4 (B) is only meaningful on the stack */
#define P_C 0x01
#define P_Z 0x02
#define P_I 0x04
#define P_D 0x08
#define P_B 0x10
#define P_U 0x20
#define P_V 0x40
#define P_N 0x80
extern uint8_t bus_rd(uint16_t addr);            /* provided by the board: one cycle */
extern void    bus_wr(uint16_t addr, uint8_t v); /* provided by the board: one cycle */
/* Execute one instruction.  Returns 0, or -1 if the opcode is not a documented one (state unchanged except pc advanced by 1, one cycle used). */
int  cpu_step(Cpu6502 *c);
/* 7-cycle NMI sequence (push pc, push p without B, set I, load vector $FFFA). */
void cpu_nmi(Cpu6502 *c);
void cpu_reset(Cpu6502 *c);   /* registers only (a=x=y=0, s=$FD, p=$24); the board loads pc and burns the reset cycles */
#endif
