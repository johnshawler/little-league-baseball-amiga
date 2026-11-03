// (libretro sets MasterVolume 10.0)
// apu_blipcmp: feeds OUR APU channel events through Mesen's own mixer arithmetic + blip_buf (the exact audio path of SoundMixer.cpp) and compares the
// resulting 48 kHz PCM against Mesen's recorded PCM sample by sample.  Proves that (events identical) => (final audio identical).
//   build: g++ -O2 -I$MESEN/Utilities -o apu_blipcmp tools/apu_blipcmp.cpp $MESEN/Utilities/blip_buf.cpp
//   usage: apu_blipcmp mine.ev(8-byte records {u32 bus cycle,u8 ch,u8 lvl,u16}) mesen.pcm(int16 stereo 48 kHz)
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "blip_buf.h"
static uint16_t sqv(int s){ return (uint16_t)((95.88*5000.0)/(8128.0/s+100.0)); }
static uint16_t tndv(int t,int n){ double o=1.0966713261*0+2.7516713261*t+1.8493587125*n; return (uint16_t)((159.79*5000.0)/(22638.0/o+100.0)); }
int main(int c,char**v){
    FILE*f=fopen(v[1],"rb"); FILE*p=fopen(v[2],"rb"); std::vector<int16_t> ref; { int16_t b[2]; while(fread(b,2,2,p)==2) ref.push_back(b[0]); }
    { fseek(p,0,SEEK_SET); long lr=0,tot=0; int16_t b[2]; while(fread(b,2,2,p)==2){ tot++; if(b[0]!=b[1]) lr++; } printf("mesen pcm: %ld stereo frames, L!=R in %ld\n",tot,lr); }
    struct R { uint32_t cc; uint8_t ch,lvl; uint16_t pad; };
    std::vector<R> ev; R r; while(fread(&r,8,1,f)==1) ev.push_back(r);
    std::stable_sort(ev.begin(),ev.end(),[](const R&a,const R&b){return a.cc<b.cc;});   // raw Mesen logs are in processing order, not time order
    blip_t* bl=blip_new(96000); blip_set_rates(bl,1789773,48000);
    int lv[4]={0,0,0,0}; int prev=0; std::vector<int16_t> out; int16_t tmp[8192]; size_t i=0; uint64_t block=0;
    const uint32_t CL=10000;
    while(i<ev.size()){
        uint64_t blk=((uint64_t)ev[i].cc+1-1)/(CL-1);   // APU cycle = cc+1; blocks are 9999 long
        while(block<blk){ blip_end_frame(bl,CL-1); int n=blip_read_samples(bl,tmp,8192,0); out.insert(out.end(),tmp,tmp+n); block++; }
        uint32_t stamp=(uint32_t)(ev[i].cc+1-block*(CL-1));
        while(i<ev.size() && (uint64_t)ev[i].cc+1-block*(CL-1)==stamp){ lv[ev[i].ch]=ev[i].lvl; i++; }
        int cur=sqv(lv[0]+lv[1])+tndv(lv[2],lv[3]);
        blip_add_delta(bl,stamp+(getenv("TOFF")?atoi(getenv("TOFF")):0),(int)((cur-prev)*10.0)); prev=cur;
    }
    blip_end_frame(bl,CL-1); { int n=blip_read_samples(bl,tmp,8192,0); out.insert(out.end(),tmp,tmp+n); }
    size_t n=std::min(out.size(),ref.size()), bad=0, first=(size_t)-1; int maxd=0;
    for(size_t k=0;k<n;k++){ int d=abs(out[k]-ref[k]); if(d){ bad++; if(first==(size_t)-1) first=k; if(d>maxd) maxd=d; } }
    printf("mine %zu samples, mesen %zu samples, compared %zu: %zu differ (first at %ld), max abs diff %d\n",out.size(),ref.size(),n,bad,(long)first,maxd);
    if(getenv("DUMP")){ FILE*o=fopen(getenv("DUMP"),"wb"); fwrite(out.data(),2,out.size(),o); fclose(o);}
    return bad!=0;
}
