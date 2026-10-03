/* Khronos-declared guest prototypes; host oracle checks distinct integer/FP values. */
/* Guest-visible SDL structs/FP returns and GL/Vulkan pointer marshalling.
 * scripts/run_thunk_compat.sh supplies small host probes for GL/Vulkan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static uint64_t thunk_dlopen(const char *path) {
    register uint64_t x0 __asm__("x0") = (uintptr_t)path;
    register uint64_t x1 __asm__("x1") = 1;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static void *sym(uint64_t lib, const char *name) {
    register uint64_t x0 __asm__("x0") = lib;
    register uint64_t x1 __asm__("x1") = (uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    if (!x0) { fprintf(stderr,"missing symbol: %s\n", name); exit(2); }
    return (void *)(uintptr_t)x0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

int main(void) {
 uint64_t lib=thunk_dlopen("libGL.so.1"); CHECK(lib);
 unsigned (*error)(void)=sym(lib,"glGetError");
 { void (*call)(unsigned,float)=sym(lib,"glAccum"); call(256u,2.25f); CHECK(error()==0); }
 { void (*call)(float,float,float,float)=sym(lib,"glClearAccum"); call(1.25f,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(float)=sym(lib,"glClearIndex"); call(1.25f); CHECK(error()==0); }
 { void (*call)(float)=sym(lib,"glEvalCoord1f"); call(1.25f); CHECK(error()==0); }
 { void (*call)(float,float)=sym(lib,"glEvalCoord2f"); call(1.25f,2.25f); CHECK(error()==0); }
 { void (*call)(float)=sym(lib,"glIndexf"); call(1.25f); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,float)=sym(lib,"glLightf"); call(256u,257u,3.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float,float)=sym(lib,"glMapGrid1f"); call(256u,2.25f,3.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float,float,unsigned,float,float)=sym(lib,"glMapGrid2f"); call(256u,2.25f,3.25f,259u,5.25f,6.25f); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,float)=sym(lib,"glMaterialf"); call(256u,257u,3.25f); CHECK(error()==0); }
 { void (*call)(float)=sym(lib,"glPassThrough"); call(1.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float)=sym(lib,"glPixelStoref"); call(256u,2.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float)=sym(lib,"glPixelTransferf"); call(256u,2.25f); CHECK(error()==0); }
 { void (*call)(float,float)=sym(lib,"glPixelZoom"); call(1.25f,2.25f); CHECK(error()==0); }
 { void (*call)(float,float)=sym(lib,"glRasterPos2f"); call(1.25f,2.25f); CHECK(error()==0); }
 { void (*call)(float,float,float)=sym(lib,"glRasterPos3f"); call(1.25f,2.25f,3.25f); CHECK(error()==0); }
 { void (*call)(float,float,float,float)=sym(lib,"glRasterPos4f"); call(1.25f,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(float,float,float,float)=sym(lib,"glRectf"); call(1.25f,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,float)=sym(lib,"glTexEnvf"); call(256u,257u,3.25f); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,float)=sym(lib,"glTexGenf"); call(256u,257u,3.25f); CHECK(error()==0); }
 { void (*call)(float,float,float,float)=sym(lib,"glVertex4f"); call(1.25f,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(unsigned,double,double)=sym(lib,"glMapGrid1d"); call(256u,-3.5,-4.5); CHECK(error()==0); }
 { void (*call)(unsigned,double,double,unsigned,double,double)=sym(lib,"glMapGrid2d"); call(256u,-3.5,-4.5,259u,-6.5,-7.5); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,double)=sym(lib,"glTexGend"); call(256u,257u,-4.5); CHECK(error()==0); }
 { void (*call)(unsigned)=sym(lib,"glActiveStencilFaceEXT"); call(256u); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned)=sym(lib,"glBindProgramARB"); call(256u,257u); CHECK(error()==0); }
 { void (*call)(double,double)=sym(lib,"glDepthBoundsEXT"); call(-2.5,-3.5); CHECK(error()==0); }
 { void (*call)(unsigned,unsigned,unsigned,unsigned)=sym(lib,"glStencilOpSeparateATI"); call(256u,257u,258u,259u); CHECK(error()==0); }
 { void (*call)(float)=sym(lib,"glTexCoord1f"); call(1.25f); CHECK(error()==0); }
 { void (*call)(float,float,float)=sym(lib,"glTexCoord3f"); call(1.25f,2.25f,3.25f); CHECK(error()==0); }
 { void (*call)(float,float,float,float)=sym(lib,"glTexCoord4f"); call(1.25f,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float)=sym(lib,"glMultiTexCoord1f"); call(256u,2.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float,float,float)=sym(lib,"glMultiTexCoord3f"); call(256u,2.25f,3.25f,4.25f); CHECK(error()==0); }
 { void (*call)(unsigned,float,float,float,float)=sym(lib,"glMultiTexCoord4f"); call(256u,2.25f,3.25f,4.25f,5.25f); CHECK(error()==0); }
 { void (*call)(float,float,float)=sym(lib,"glSecondaryColor3f"); call(1.25f,2.25f,3.25f); CHECK(error()==0); }

 for(unsigned variant=0;variant<4;variant++) for(unsigned target=0;target<9;target++) {
  static const unsigned components[]={4,1,3,1,2,3,4,3,4};
  int two=variant>=2, dbl=variant&1, us=19, uo=1024, vs=5, vo=7;
  size_t count=(uo-1)*us+(two?(vo-1)*vs:0)+components[target];
  size_t bytes=count*(dbl?8:4), pages=(bytes+4095)&~(size_t)4095;
  unsigned char *base=mmap((void*)0x6400000000ull,pages+4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
  CHECK(base!=MAP_FAILED); CHECK(mprotect(base+pages,4096,PROT_NONE)==0);
  void *points=base+pages-bytes;
  for(size_t i=0;i<count;i++) { if(dbl) ((double*)points)[i]=i+0.125; else ((float*)points)[i]=i+0.125f; }
  CHECK(mprotect(base,pages,PROT_READ)==0);
  const char *names[]={"glMap1f","glMap1d","glMap2f","glMap2d"};
  void *call=sym(lib,names[variant]);
  unsigned enum_target=(two?0xdb0:0xd90)+target;
  if(variant==0) ((void(*)(unsigned,float,float,int,int,const float*))call)(enum_target,-1.25f,2.5f,us,uo,points);
  if(variant==1) ((void(*)(unsigned,double,double,int,int,const double*))call)(enum_target,-1.25,2.5,us,uo,points);
  if(variant==2) ((void(*)(unsigned,float,float,int,int,float,float,int,int,const float*))call)(enum_target,-1.25f,2.5f,us,uo,-3.5f,4.75f,vs,vo,points);
  if(variant==3) ((void(*)(unsigned,double,double,int,int,double,double,int,int,const double*))call)(enum_target,-1.25,2.5,us,uo,-3.5,4.75,vs,vo,points);
  CHECK(error()==0);
  if(variant==3) { ((void(*)(unsigned,double,double,int,int,double,double,int,int,const double*))call)(enum_target,0,0,us,-1,0,0,vs,vo,(void*)1); CHECK(error()==0x501); }
  CHECK(munmap(base,pages+4096)==0);
 }
 unsigned char *base=mmap((void*)0x6400000000ull,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 CHECK(base!=MAP_FAILED); CHECK(mprotect(base+4096,4096,PROT_NONE)==0);
 uint32_t *ids=(uint32_t*)base;
 void (*gen)(int,uint32_t*)=sym(lib,"glGenProgramsARB"); gen(1024,ids); CHECK(error()==0);
 for(int i=0;i<1024;i++) CHECK(ids[i]==0x1200u+i);
 float *vector=(float*)(base+4096-16); for(int i=0;i<4;i++) vector[i]=i+0.25f;
 CHECK(mprotect(base,4096,PROT_READ)==0);
 void (*env)(unsigned,unsigned,const float*)=sym(lib,"glProgramEnvParameter4fvARB");
 void (*local)(unsigned,unsigned,const float*)=sym(lib,"glProgramLocalParameter4fvARB");
 env(0x8620,3,vector); CHECK(error()==0); local(0x8620,3,vector); CHECK(error()==0);
 CHECK(mprotect(base,4096,PROT_READ|PROT_WRITE)==0);
 unsigned char *data=base+4096-768; for(int i=0;i<768;i++) data[i]=(unsigned char)i;
 CHECK(mprotect(base,4096,PROT_READ)==0);
 void (*color)(unsigned,unsigned,int,unsigned,unsigned,const void*)=sym(lib,"glColorTableEXT");
 color(0x81fb,0x1907,256,0x1907,0x1401,data); CHECK(error()==0);
 void (*program)(unsigned,unsigned,int,const void*)=sym(lib,"glProgramStringARB");
 program(0x8620,0x8875,768,data); CHECK(error()==0);
 CHECK(munmap(base,8192)==0);
 puts("Doom 3 legacy GL ABI and guarded buffers passed"); return 0;
}
