#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#pragma pack(push,1)
typedef struct { char t; uint8_t v; uint16_t a; int32_t abs; uint32_t cyc; uint32_t fr; } R;
#pragma pack(pop)
int main(int c,char**v){ FILE*f=fopen(v[1],"rb"); static R r[1<<20]; size_t n;
 uint32_t nmi_cyc=0; int have=0; uint32_t last_nmi=0;
 // phase histogram (offset since last NMI entry at pc cd85, in 1/30 frame bins)
 long hist[16][32]; memset(hist,0,sizeof hist);
 const uint16_t regs[16]={0x2000,0x2001,0x2005,0x2006,0x2007,0x2004,0x2003,0x4014,0x4015,0xA000,0xC000,0xE000,0x2002,0x4016,0x4000,0x400C};
 long nframes=0; long idle=0; long cur_idle=0; double busy_max=0,busy_sum=0; double bmax_list=0; long frames_busy=0;
 uint32_t prevc=0; int inidle=0; uint32_t idle_start=0; long idle_acc=0; long hist_busy[40]; memset(hist_busy,0,sizeof hist_busy);
 uint64_t cyc64=0; int pidle=0; uint64_t pcy=0; uint32_t lastc=0; uint64_t lastnmi64=0; uint64_t idlecyc=0; uint64_t idleatnmi=0;
 double maxbusy=0; double sumbusy=0; long nb=0;
 while((n=fread(r,sizeof(R),1<<20,f))>0) for(size_t i=0;i<n;i++){ R*x=&r[i];
   if(x->cyc<lastc && lastc-x->cyc>0x80000000u) cyc64+=1ull<<32; lastc=x->cyc; uint64_t cy=cyc64+x->cyc;
   if(x->t=='X'){
     if(x->a==0xcd85){ // NMI entry
        if(have){ uint64_t per=cy-lastnmi64; double busy=(double)(per-(idlecyc-idleatnmi)); if(per>29000&&per<31000){ sumbusy+=busy; nb++; if(busy>maxbusy)maxbusy=busy; int b=(int)(busy/1000); if(b>39)b=39; hist_busy[b]++; } }
        lastnmi64=cy; idleatnmi=idlecyc; have=1; }
     if(pidle) idlecyc+=cy-pcy;
     pidle=(x->a==0xcc09||x->a==0xcc0c||x->a==0xcdf9||x->a==0xcdfc||x->a==0xcdfe||x->a==0xce01||x->a==0xce03); pcy=cy;
   }
   if(have && x->t!='X'){ uint32_t off=(uint32_t)(cy-lastnmi64); int b=off*30/29780; if(b>31)b=31; int idx=-1; uint16_t a=x->a; if(x->t=='W'&&a>=0x8000) a&=0xE000; for(int k=0;k<16;k++) if(regs[k]==a) idx=k; if(idx>=0 && (x->t=='W' || idx==12)) hist[idx][b]++; }
 }
 printf("frames measured %ld, mean busy cycles/frame %.0f, max %.0f (NTSC frame 29780.5)\n",nb,sumbusy/nb,maxbusy);
 printf("busy histogram (k cycles: frames):"); for(int i=0;i<40;i++) if(hist_busy[i]) printf(" %d:%ld",i,hist_busy[i]); printf("\n");
 const char*nmv[16]={"W2000","W2001","W2005","W2006","W2007","W2004","W2003","W4014","W4015","W-CHR0 A000","W-CHR1 C000","W-PRG E000","R2002","W4016","W4000","W400C"};
 printf("write/poll phase histogram: 30 bins of the frame since NMI entry\n"); for(int k=0;k<16;k++){ printf("%-12s",nmv[k]); for(int b=0;b<30;b++) printf("%6ld",hist[k][b]); printf("\n"); }
}
