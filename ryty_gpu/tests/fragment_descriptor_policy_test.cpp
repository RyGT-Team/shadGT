#include "video_core/renderer_vulkan/fragment_descriptor_policy.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
int main(){unsigned cases=0;for(unsigned vendor:{0x10deu,0x1002u,0x8086u,0u})for(bool native:{false,true})for(const char* value:{(const char*)nullptr,"","0","1","00","invalid"}){assert(Vulkan::CacheFragmentDescriptors(vendor,native,value)==(vendor==0x10de&&native&&(!value||value[0]!='0'||value[1]!='\0')));++cases;}printf("PASS %u vendor/native/default/override cases\n",cases);}
