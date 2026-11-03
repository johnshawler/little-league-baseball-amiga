/* apu_gentab: prints the mixer tables of runtime/apu.c with the exact floating point formulas of Mesen SoundMixer::GetOutputVolume (DMC level 0). gcc -o gentab tools/apu_gentab.c && ./gentab */
#include <stdio.h>
#include <stdint.h>
int main(void){
  printf("static const uint16_t sq_mix[31] = {");
  for(int s=0;s<31;s++){ double so=s; uint16_t v=(uint16_t)((95.88*5000.0)/(8128.0/so+100.0)); printf("%s%u",s?",":"",v);} printf("};\n");
  printf("static const uint16_t tnd_mix[256] = {");
  for(int t=0;t<16;t++)for(int n=0;n<16;n++){ double d=0; double o=1.0966713261*d+2.7516713261*t+1.8493587125*n; uint16_t v=(uint16_t)((159.79*5000.0)/(22638.0/o+100.0)); printf("%s%u",(t||n)?",":"",v);} printf("};\n");
}
