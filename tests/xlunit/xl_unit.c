/* Per-instruction tests of the translated code against runtime/cpu6502.c: random registers/flags/RAM, compares registers, flags, RAM, stack and
 * the number of cycles consumed.  Runs on m68k (qemu-m68k): xl_unit <dir with prg.bin, tests.txt> [trials]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/cpu6502.h"

typedef struct {
    uint8_t nzc, vflag, misc, _p; int16_t adj; uint16_t ptmp;
    uint32_t d[6]; void *entry; void *saved_sp; uint8_t *ram; uint8_t *win; void *tabwin, *tabfix; uint8_t *nztab; uint8_t *fixbase;
} XCtx;
extern void xl_call(XCtx *c);
extern uint32_t tab_win0[], tab_fix[];
extern void *F_C100;      /* unused: entries are looked up by label name through the table */
static uint8_t prg[131072]; static uint8_t ram[2048]; static uint8_t nztab[16]={0,0,0,0,2,0,0,0,0x80,0,0,0,0x82,0,0,0};
static int ref_cycles, errors_mem;

static uint8_t mem_rd(uint16_t a){
    if(a<0x2000) return ram[a&0x7FF];
    if(a>=0x8000&&a<0xC000) return prg[a-0x8000];
    if(a>=0xC000) return prg[7*16384+a-0xC000];
    errors_mem++; return 0;
}
uint8_t xl_rd_c(uint32_t a){ return mem_rd((uint16_t)a); }
void xl_wr_c(uint32_t a,uint32_t v){ if(a<0x2000) ram[a&0x7FF]=(uint8_t)v; else errors_mem++; }
uint8_t bus_rd(uint16_t a){ ref_cycles++; return mem_rd(a); }
void bus_wr(uint16_t a,uint8_t v){ ref_cycles++; if(a<0x2000) ram[a&0x7FF]=v; else errors_mem++; }

static uint32_t rng=1; static uint32_t rnd(void){ rng=rng*1664525u+1013904223u; return rng>>8; }

int main(int argc,char**argv){
    char path[512]; snprintf(path,sizeof path,"%s/prg.bin",argv[1]); FILE*f=fopen(path,"rb"); if(!f||fread(prg,1,131072,f)!=131072){ perror("prg"); return 2; } fclose(f);
    int trials=argc>2?atoi(argv[2]):100;
    snprintf(path,sizeof path,"%s/tests.txt",argv[1]); f=fopen(path,"r"); char line[1024];
    long ntests=0,nfail=0,nruns=0;
    while(fgets(line,sizeof line,f)){
        unsigned addr,n; char kind[16]; int off; if(sscanf(line,"%u %u %15s %n",&addr,&n,kind,&off)<3) continue;
        uint8_t code[16]; char*p=line+off; for(unsigned i=0;i<n;i++) code[i]=(uint8_t)strtol(p,&p,16);
        char*name=strchr(line,'|')+1; name[strcspn(name,"\n")]=0;
        /* entry point: the code was placed at `addr` in the fixed bank; find the translated label through the table */
        uint32_t *ent=&tab_fix[addr-0xC000]; ntests++; int failed=0; if(getenv("XLV")){ fprintf(stderr,"%s\n",name); }
        for(int t=0;t<trials && failed<3;t++){
            XCtx c; memset(&c,0,sizeof c);
            for(int i=0;i<2048;i++) ram[i]=(uint8_t)rnd();
            uint8_t A=rnd(),X=rnd(),Y=rnd(),S=(uint8_t)(rnd()%200+20),P=(uint8_t)((rnd()&0xCF)|0x20);
            if(t==0){ X=0xFF; Y=0xFF; } if(t==1){ X=0; Y=0; } if(t==2){ X=0x80; Y=0x80; }
            /* pointer set-up for (zp),Y */
            if(!strcmp(kind,"izy_r")||!strcmp(kind,"izy_w")){
                unsigned zp=(!strcmp(name,"") || strncmp(name,"SEQ",3)) ? code[1] : 0x20; uint16_t ptr;
                switch(rnd()%3){ case 0: ptr=0x0200+rnd()%0x400; break; case 1: ptr=0x8000+rnd()%0x3E00; break; default: ptr=0xD000+rnd()%0x2E00; }
                if(!strcmp(kind,"izy_w")) ptr=0x0200+rnd()%0x500;
                if(t==0) ptr=(ptr&0xFF00)|0xF0|(rnd()&0xF);        /* force page crossing with Y=255 */
                ram[zp]=ptr&0xFF; ram[(zp+1)&0xFF]=ptr>>8;
            }
            /* stack: return address $FFEF (RTS -> $FFF0), or for RTI: P, PCL, PCH */
            uint8_t sp=S;
            if(!strcmp(kind,"rti")){ ram[0x100+sp]=0xFF; sp--; ram[0x100+sp]=0xF0; sp--; ram[0x100+sp]=(uint8_t)((rnd()&0xCF)|0x20); sp--; }
            else if(!strcmp(kind,"jmpind")){ unsigned a=code[1]|code[2]<<8; ram[a&0x7FF]=0xF0; ram[(a+1)&0x7FF]=0xFF; ram[0x100+sp]=0xFF; sp--; ram[0x100+sp]=0xEF; sp--; }
            else { ram[0x100+sp]=0xFF; sp--; ram[0x100+sp]=0xEF; sp--; if(!strcmp(kind,"pull")){ ram[0x100+sp]=(uint8_t)rnd(); sp--; } if(!strcmp(kind,"txs")) X=sp; }
            uint8_t ram0[2048]; memcpy(ram0,ram,2048);
            /* ---- translated run */
            c.nzc=P&0x82; c.vflag=P&0x40; c.misc=P&0x0C; c.d[0]=A; c.d[1]=X; c.d[2]=Y; c.d[3]=sp; c.d[4]=(P&1)?0xFF:0; c.d[5]=1000000;
            c.entry=ent; c.ram=ram; c.win=prg; c.tabwin=(uint8_t*)tab_win0-0x20000; c.tabfix=(uint8_t*)tab_fix-0x30000; c.nztab=nztab; c.fixbase=prg+7*16384-0x4000;
            c.entry=(void*)(uintptr_t)*ent;
            errors_mem=0; xl_call(&c); nruns++;
            uint8_t tram[2048]; memcpy(tram,ram,2048);
            int tm_err=errors_mem;
            /* ---- reference run */
            memcpy(ram,ram0,2048); Cpu6502 r={0xFFFF,A,X,Y,sp,P}; r.pc=(uint16_t)addr; ref_cycles=0; errors_mem=0;
            for(int s=0;s<8 && r.pc!=0xFFF0;s++) if(cpu_step(&r)<0){ printf("ref illegal\n"); break; }
            int used=1000000-(int)c.d[5];
            uint8_t tA=c.d[0],tX=c.d[1],tY=c.d[2],tS=c.d[3];
            uint8_t tP=(uint8_t)((c.nzc&0x82)|((c.d[4]&0xFF)?1:0)|(c.vflag&0x40)|(c.misc&0x0C));
            int ok=(tA==r.a)&&(tX==r.x)&&(tY==r.y)&&(tS==r.s)&&((tP&0xCF)==(r.p&0xCF))&&(used==ref_cycles)&&!memcmp(tram,ram,2048)&&!tm_err&&!errors_mem;
            if(!ok){ failed++; nfail++;
                printf("FAIL %-18s [%s] A %02x/%02x X %02x/%02x Y %02x/%02x S %02x/%02x P %02x/%02x cyc %d/%d%s%s init A=%02x X=%02x Y=%02x P=%02x\n",name,kind,tA,r.a,tX,r.x,tY,r.y,tS,r.s,tP&0xCF,r.p&0xCF,used,ref_cycles,
                    memcmp(tram,ram,2048)?" RAM":"",(tm_err||errors_mem)?" MEMERR":"",A,X,Y,P);
            }
        }
    }
    printf("xl_unit: %ld test programs, %ld runs, %ld failures\n",ntests,nruns,nfail);
    return nfail!=0;
}
