#include "cpu6502.h"
#define RD(a) bus_rd((uint16_t)(a))
#define WR(a,v) bus_wr((uint16_t)(a),(uint8_t)(v))
#define NZ(v) do{ uint8_t _v=(v); c->p=(uint8_t)((c->p&~(P_N|P_Z))|(_v&0x80)|(_v?0:P_Z)); }while(0)
#define FL(m,cond) do{ if(cond) c->p|=(m); else c->p&=(uint8_t)~(m); }while(0)

static void push(Cpu6502*c,uint8_t v){ WR(0x100|c->s,v); c->s--; }
static uint8_t pull(Cpu6502*c){ c->s++; return RD(0x100|c->s); }

enum { M_IMP,M_ACC,M_IMM,M_ZP,M_ZPX,M_ZPY,M_ABS,M_ABX,M_ABY,M_IZX,M_IZY,M_REL,M_IND };
/* kinds: 0 read, 1 write, 2 rmw */
static uint16_t ea(Cpu6502*c,int mode,int kind){
  uint16_t a,lo,hi,t;
  switch(mode){
  case M_ZP: return RD(c->pc++);
  case M_ZPX: a=RD(c->pc++); RD(a); return (uint8_t)(a+c->x);
  case M_ZPY: a=RD(c->pc++); RD(a); return (uint8_t)(a+c->y);
  case M_ABS: lo=RD(c->pc++); hi=RD(c->pc++); return lo|hi<<8;
  case M_ABX: case M_ABY: lo=RD(c->pc++); hi=RD(c->pc++);
    t=lo+(mode==M_ABX?c->x:c->y);
    if((t&0x100)||kind) RD((hi<<8)|(t&0xFF));
    return (uint16_t)((hi<<8)+lo+(mode==M_ABX?c->x:c->y));
  case M_IZX: a=RD(c->pc++); RD(a); a=(uint8_t)(a+c->x); lo=RD(a); hi=RD((uint8_t)(a+1)); return lo|hi<<8;
  case M_IZY: a=RD(c->pc++); lo=RD(a); hi=RD((uint8_t)(a+1)); t=lo+c->y;
    if((t&0x100)||kind) RD((hi<<8)|(t&0xFF));
    return (uint16_t)((hi<<8)+lo+c->y);
  }
  return 0;
}
static void adc(Cpu6502*c,uint8_t m){
  unsigned s=c->a+m+(c->p&P_C);
  FL(P_V,(~(c->a^m)&(c->a^s)&0x80)); FL(P_C,s>0xFF); c->a=(uint8_t)s; NZ(c->a);
}
static void cmpr(Cpu6502*c,uint8_t r,uint8_t m){ FL(P_C,r>=m); NZ((uint8_t)(r-m)); }
static uint8_t asl(Cpu6502*c,uint8_t v){ FL(P_C,v&0x80); v<<=1; NZ(v); return v; }
static uint8_t lsr(Cpu6502*c,uint8_t v){ FL(P_C,v&1); v>>=1; NZ(v); return v; }
static uint8_t rol(Cpu6502*c,uint8_t v){ uint8_t o=c->p&P_C; FL(P_C,v&0x80); v=(uint8_t)(v<<1|o); NZ(v); return v; }
static uint8_t ror(Cpu6502*c,uint8_t v){ uint8_t o=c->p&P_C; FL(P_C,v&1); v=(uint8_t)(v>>1|o<<7); NZ(v); return v; }

/* group tables: op -> mode */
static const int8_t MODE[256]={
/*0*/ M_IMP,M_IZX,-1,-1,-1,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_ACC,-1,-1,M_ABS,M_ABS,-1,
/*1*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1,
/*2*/ M_ABS,M_IZX,-1,-1,M_ZP,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_ACC,-1,M_ABS,M_ABS,M_ABS,-1,
/*3*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1,
/*4*/ M_IMP,M_IZX,-1,-1,-1,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_ACC,-1,M_ABS,M_ABS,M_ABS,-1,
/*5*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1,
/*6*/ M_IMP,M_IZX,-1,-1,-1,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_ACC,-1,M_IND,M_ABS,M_ABS,-1,
/*7*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1,
/*8*/ -1,M_IZX,-1,-1,M_ZP,M_ZP,M_ZP,-1,M_IMP,-1,M_IMP,-1,M_ABS,M_ABS,M_ABS,-1,
/*9*/ M_REL,M_IZY,-1,-1,M_ZPX,M_ZPX,M_ZPY,-1,M_IMP,M_ABY,M_IMP,-1,-1,M_ABX,-1,-1,
/*A*/ M_IMM,M_IZX,M_IMM,-1,M_ZP,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_IMP,-1,M_ABS,M_ABS,M_ABS,-1,
/*B*/ M_REL,M_IZY,-1,-1,M_ZPX,M_ZPX,M_ZPY,-1,M_IMP,M_ABY,M_IMP,-1,M_ABX,M_ABX,M_ABY,-1,
/*C*/ M_IMM,M_IZX,-1,-1,M_ZP,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_IMP,-1,M_ABS,M_ABS,M_ABS,-1,
/*D*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1,
/*E*/ M_IMM,M_IZX,-1,-1,M_ZP,M_ZP,M_ZP,-1,M_IMP,M_IMM,M_IMP,-1,M_ABS,M_ABS,M_ABS,-1,
/*F*/ M_REL,M_IZY,-1,-1,-1,M_ZPX,M_ZPX,-1,M_IMP,M_ABY,-1,-1,-1,M_ABX,M_ABX,-1 };

int cpu_step(Cpu6502*c){
  uint8_t op=RD(c->pc++); int mode=MODE[op]; uint16_t a; uint8_t m,t;
  /* control / implied instructions first */
  switch(op){
  case 0x00: RD(c->pc); c->pc++; push(c,(uint8_t)(c->pc>>8)); push(c,(uint8_t)c->pc); push(c,c->p|P_B|P_U); c->p|=P_I;
             a=RD(0xFFFE); a|=RD(0xFFFF)<<8; c->pc=a; return 0;
  case 0x20: { uint16_t lo=RD(c->pc++); RD(0x100|c->s); push(c,(uint8_t)(c->pc>>8)); push(c,(uint8_t)c->pc); c->pc=lo|RD(c->pc)<<8; return 0; }
  case 0x40: RD(c->pc); RD(0x100|c->s); c->p=(pull(c)&~P_B)|P_U; a=pull(c); a|=pull(c)<<8; c->pc=a; return 0;
  case 0x60: RD(c->pc); RD(0x100|c->s); a=pull(c); a|=pull(c)<<8; RD(a); c->pc=a+1; return 0;
  case 0x48: RD(c->pc); push(c,c->a); return 0;
  case 0x08: RD(c->pc); push(c,c->p|P_B|P_U); return 0;
  case 0x68: RD(c->pc); RD(0x100|c->s); c->a=pull(c); NZ(c->a); return 0;
  case 0x28: RD(c->pc); RD(0x100|c->s); c->p=(pull(c)&~P_B)|P_U; return 0;
  case 0x4C: { uint16_t lo=RD(c->pc++); c->pc=lo|RD(c->pc)<<8; return 0; }
  case 0x6C: { uint16_t lo=RD(c->pc++); uint16_t hi=RD(c->pc++); uint16_t p=lo|hi<<8; a=RD(p); a|=RD((p&0xFF00)|((p+1)&0xFF))<<8; c->pc=a; return 0; }
  case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: {
     int8_t off=(int8_t)RD(c->pc++); int cond;
     switch(op){ case 0x10: cond=!(c->p&P_N); break; case 0x30: cond=c->p&P_N; break; case 0x50: cond=!(c->p&P_V); break; case 0x70: cond=c->p&P_V; break;
                 case 0x90: cond=!(c->p&P_C); break; case 0xB0: cond=c->p&P_C; break; case 0xD0: cond=!(c->p&P_Z); break; default: cond=c->p&P_Z; }
     if(cond){ RD(c->pc); uint16_t np=c->pc+off; if((np^c->pc)&0xFF00) RD((c->pc&0xFF00)|(np&0xFF)); c->pc=np; }
     return 0; }
  case 0x18: RD(c->pc); c->p&=~P_C; return 0;  case 0x38: RD(c->pc); c->p|=P_C; return 0;
  case 0x58: RD(c->pc); c->p&=~P_I; return 0;  case 0x78: RD(c->pc); c->p|=P_I; return 0;
  case 0xB8: RD(c->pc); c->p&=~P_V; return 0;  case 0xD8: RD(c->pc); c->p&=~P_D; return 0; case 0xF8: RD(c->pc); c->p|=P_D; return 0;
  case 0xAA: RD(c->pc); c->x=c->a; NZ(c->x); return 0; case 0xA8: RD(c->pc); c->y=c->a; NZ(c->y); return 0;
  case 0x8A: RD(c->pc); c->a=c->x; NZ(c->a); return 0; case 0x98: RD(c->pc); c->a=c->y; NZ(c->a); return 0;
  case 0xBA: RD(c->pc); c->x=c->s; NZ(c->x); return 0; case 0x9A: RD(c->pc); c->s=c->x; return 0;
  case 0xE8: RD(c->pc); c->x++; NZ(c->x); return 0; case 0xC8: RD(c->pc); c->y++; NZ(c->y); return 0;
  case 0xCA: RD(c->pc); c->x--; NZ(c->x); return 0; case 0x88: RD(c->pc); c->y--; NZ(c->y); return 0;
  case 0xEA: RD(c->pc); return 0;
  case 0x0A: RD(c->pc); c->a=asl(c,c->a); return 0; case 0x4A: RD(c->pc); c->a=lsr(c,c->a); return 0;
  case 0x2A: RD(c->pc); c->a=rol(c,c->a); return 0; case 0x6A: RD(c->pc); c->a=ror(c,c->a); return 0;
  }
  if(mode<0){ return -1; }
  /* read-type */
  switch(op){
  case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x15: case 0x19: case 0x1D: /* ORA */
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); c->a|=m; NZ(c->a); return 0;
  case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x35: case 0x39: case 0x3D:
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); c->a&=m; NZ(c->a); return 0;
  case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x55: case 0x59: case 0x5D:
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); c->a^=m; NZ(c->a); return 0;
  case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x75: case 0x79: case 0x7D:
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); adc(c,m); return 0;
  case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF5: case 0xF9: case 0xFD:
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); adc(c,(uint8_t)~m); return 0;
  case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD5: case 0xD9: case 0xDD:
    m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); cmpr(c,c->a,m); return 0;
  case 0xE0: case 0xE4: case 0xEC: m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); cmpr(c,c->x,m); return 0;
  case 0xC0: case 0xC4: case 0xCC: m= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); cmpr(c,c->y,m); return 0;
  case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB5: case 0xB9: case 0xBD:
    c->a= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); NZ(c->a); return 0;
  case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE: c->x= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); NZ(c->x); return 0;
  case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC: c->y= mode==M_IMM? RD(c->pc++) : RD(ea(c,mode,0)); NZ(c->y); return 0;
  case 0x24: case 0x2C: m=RD(ea(c,mode,0)); FL(P_Z,!(c->a&m)); FL(P_N,m&0x80); FL(P_V,m&0x40); return 0;
  /* stores */
  case 0x81: case 0x85: case 0x8D: case 0x91: case 0x95: case 0x99: case 0x9D: a=ea(c,mode,1); WR(a,c->a); return 0;
  case 0x86: case 0x8E: case 0x96: a=ea(c,mode,1); WR(a,c->x); return 0;
  case 0x84: case 0x8C: case 0x94: a=ea(c,mode,1); WR(a,c->y); return 0;
  /* RMW */
  case 0x06: case 0x0E: case 0x16: case 0x1E: a=ea(c,mode,2); t=RD(a); WR(a,t); WR(a,asl(c,t)); return 0;
  case 0x46: case 0x4E: case 0x56: case 0x5E: a=ea(c,mode,2); t=RD(a); WR(a,t); WR(a,lsr(c,t)); return 0;
  case 0x26: case 0x2E: case 0x36: case 0x3E: a=ea(c,mode,2); t=RD(a); WR(a,t); WR(a,rol(c,t)); return 0;
  case 0x66: case 0x6E: case 0x76: case 0x7E: a=ea(c,mode,2); t=RD(a); WR(a,t); WR(a,ror(c,t)); return 0;
  case 0xE6: case 0xEE: case 0xF6: case 0xFE: a=ea(c,mode,2); t=RD(a); WR(a,t); t++; NZ(t); WR(a,t); return 0;
  case 0xC6: case 0xCE: case 0xD6: case 0xDE: a=ea(c,mode,2); t=RD(a); WR(a,t); t--; NZ(t); WR(a,t); return 0;
  }
  return -1;
}
void cpu_nmi(Cpu6502*c){
  RD(c->pc); RD(c->pc); push(c,(uint8_t)(c->pc>>8)); push(c,(uint8_t)c->pc); push(c,(uint8_t)((c->p&~P_B)|P_U)); c->p|=P_I;
  uint16_t a=RD(0xFFFA); a|=RD(0xFFFB)<<8; c->pc=a;
}
void cpu_reset(Cpu6502*c){ c->a=c->x=c->y=0; c->s=0xFD; c->p=P_I|P_U; c->pc=0; }
