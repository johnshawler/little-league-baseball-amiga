// minimal libretro host: host core.so rom.nes frames script
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libretro.h"
static retro_environment_t env;
static unsigned buttons; static FILE*vid, *aud; static unsigned fmt=RETRO_PIXEL_FORMAT_0RGB1555; static long fr;
static bool envcb(unsigned cmd, void*d){
 switch(cmd){
 case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt=*(unsigned*)d; return true;
 case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)d="/tmp"; return true;
 case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)d=true; return true;
 default: return false;}}
static void videocb(const void*d,unsigned w,unsigned h,size_t p){ if(!d)return; static int dumped; 
 if(vid && getenv("DUMPFRAMES")){ fprintf(vid,"%ld %u %u %zu %u\n",fr,w,h,p,fmt); for(unsigned y=0;y<h;y++) fwrite((char*)d+y*p,1,w*(fmt==RETRO_PIXEL_FORMAT_XRGB8888?4:2),vid);} }
static void audiocb(int16_t l,int16_t r){}
static size_t audiobatch(const int16_t*d,size_t n){ if(aud) fwrite(d,4,n,aud); return n;}
static void pollcb(void){}
static int16_t statecb(unsigned port,unsigned dev,unsigned idx,unsigned id){ if(port==0&&dev==RETRO_DEVICE_JOYPAD) return (buttons>>id)&1; return 0;}
int main(int c,char**v){
 void*h=dlopen(v[1],RTLD_NOW); if(!h){puts(dlerror());return 1;}
#define F(n) void (*n)() = (void(*)())dlsym(h,#n)
 void(*set_env)(void*)=dlsym(h,"retro_set_environment"); void(*set_v)(void*)=dlsym(h,"retro_set_video_refresh");
 void(*set_a)(void*)=dlsym(h,"retro_set_audio_sample"); void(*set_ab)(void*)=dlsym(h,"retro_set_audio_sample_batch");
 void(*set_p)(void*)=dlsym(h,"retro_set_input_poll"); void(*set_s)(void*)=dlsym(h,"retro_set_input_state");
 void(*init)(void)=dlsym(h,"retro_init"); int(*load)(struct retro_game_info*)=dlsym(h,"retro_load_game"); void(*run)(void)=dlsym(h,"retro_run");
 void*(*getmem)(unsigned)=dlsym(h,"retro_get_memory_data"); size_t(*memsz)(unsigned)=dlsym(h,"retro_get_memory_size");
 void(*sysav)(struct retro_system_av_info*)=dlsym(h,"retro_get_system_av_info");
 set_env(envcb);init();set_v(videocb);set_a(audiocb);set_ab(audiobatch);set_p(pollcb);set_s(statecb);
 FILE*f=fopen(v[2],"rb");fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);uint8_t*rom=malloc(n);fread(rom,1,n,f);
 struct retro_game_info gi={v[2],rom,n,0}; if(!load(&gi)){puts("load fail");return 1;}
 struct retro_system_av_info av; sysav(&av); fprintf(stderr,"fps %.4f rate %.1f %ux%u\n",av.timing.fps,av.timing.sample_rate,av.geometry.base_width,av.geometry.base_height);
 long frames=atol(v[3]); FILE*sc=fopen(v[4],"r"); // script: "frame mask" lines
 long sf[4096]; unsigned sm[4096]; int ns=0; while(sc&&fscanf(sc,"%ld %x",&sf[ns],&sm[ns])==2)ns++;
 vid=fopen(getenv("VID")?getenv("VID"):"/dev/null","wb"); aud=getenv("AUD")?fopen(getenv("AUD"),"wb"):0;
 int si=0; for(fr=0;fr<frames;fr++){ while(si<ns&&sf[si]<=fr){buttons=sm[si];si++;} run();
   if(getenv("RAMDUMP")&&fr%60==0){FILE*r=fopen(getenv("RAMDUMP"),fr?"ab":"wb");void*m=getmem(RETRO_MEMORY_SYSTEM_RAM);fwrite(m,1,0x800,r);fclose(r);} }
 fclose(vid); if(aud)fclose(aud); return 0;}
