/* Host ABI oracles for the Doom 3 legacy GL batch. No GPU needed. */
#include <stdint.h>
#include <stddef.h>
static unsigned failure;
unsigned glGetError(void) { unsigned r=failure; failure=0; return r; }
void glAccum(unsigned a0,float a1) { if(a0!=256u || a1!=2.25f) failure=0x502; }
void glClearAccum(float a0,float a1,float a2,float a3) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glClearIndex(float a0) { if(a0!=1.25f) failure=0x502; }
void glEvalCoord1f(float a0) { if(a0!=1.25f) failure=0x502; }
void glEvalCoord2f(float a0,float a1) { if(a0!=1.25f || a1!=2.25f) failure=0x502; }
void glIndexf(float a0) { if(a0!=1.25f) failure=0x502; }
void glLightf(unsigned a0,unsigned a1,float a2) { if(a0!=256u || a1!=257u || a2!=3.25f) failure=0x502; }
void glMapGrid1f(unsigned a0,float a1,float a2) { if(a0!=256u || a1!=2.25f || a2!=3.25f) failure=0x502; }
void glMapGrid2f(unsigned a0,float a1,float a2,unsigned a3,float a4,float a5) { if(a0!=256u || a1!=2.25f || a2!=3.25f || a3!=259u || a4!=5.25f || a5!=6.25f) failure=0x502; }
void glMaterialf(unsigned a0,unsigned a1,float a2) { if(a0!=256u || a1!=257u || a2!=3.25f) failure=0x502; }
void glPassThrough(float a0) { if(a0!=1.25f) failure=0x502; }
void glPixelStoref(unsigned a0,float a1) { if(a0!=256u || a1!=2.25f) failure=0x502; }
void glPixelTransferf(unsigned a0,float a1) { if(a0!=256u || a1!=2.25f) failure=0x502; }
void glPixelZoom(float a0,float a1) { if(a0!=1.25f || a1!=2.25f) failure=0x502; }
void glRasterPos2f(float a0,float a1) { if(a0!=1.25f || a1!=2.25f) failure=0x502; }
void glRasterPos3f(float a0,float a1,float a2) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f) failure=0x502; }
void glRasterPos4f(float a0,float a1,float a2,float a3) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glRectf(float a0,float a1,float a2,float a3) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glTexEnvf(unsigned a0,unsigned a1,float a2) { if(a0!=256u || a1!=257u || a2!=3.25f) failure=0x502; }
void glTexGenf(unsigned a0,unsigned a1,float a2) { if(a0!=256u || a1!=257u || a2!=3.25f) failure=0x502; }
void glVertex4f(float a0,float a1,float a2,float a3) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glMapGrid1d(unsigned a0,double a1,double a2) { if(a0!=256u || a1!=-3.5 || a2!=-4.5) failure=0x502; }
void glMapGrid2d(unsigned a0,double a1,double a2,unsigned a3,double a4,double a5) { if(a0!=256u || a1!=-3.5 || a2!=-4.5 || a3!=259u || a4!=-6.5 || a5!=-7.5) failure=0x502; }
void glTexGend(unsigned a0,unsigned a1,double a2) { if(a0!=256u || a1!=257u || a2!=-4.5) failure=0x502; }
void glActiveStencilFaceEXT(unsigned a0) { if(a0!=256u) failure=0x502; }
void glBindProgramARB(unsigned a0,unsigned a1) { if(a0!=256u || a1!=257u) failure=0x502; }
void glDepthBoundsEXT(double a0,double a1) { if(a0!=-2.5 || a1!=-3.5) failure=0x502; }
void glStencilOpSeparateATI(unsigned a0,unsigned a1,unsigned a2,unsigned a3) { if(a0!=256u || a1!=257u || a2!=258u || a3!=259u) failure=0x502; }
void glTexCoord1f(float a0) { if(a0!=1.25f) failure=0x502; }
void glTexCoord3f(float a0,float a1,float a2) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f) failure=0x502; }
void glTexCoord4f(float a0,float a1,float a2,float a3) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glMultiTexCoord1f(unsigned a0,float a1) { if(a0!=256u || a1!=2.25f) failure=0x502; }
void glMultiTexCoord3f(unsigned a0,float a1,float a2,float a3) { if(a0!=256u || a1!=2.25f || a2!=3.25f || a3!=4.25f) failure=0x502; }
void glMultiTexCoord4f(unsigned a0,float a1,float a2,float a3,float a4) { if(a0!=256u || a1!=2.25f || a2!=3.25f || a3!=4.25f || a4!=5.25f) failure=0x502; }
void glSecondaryColor3f(float a0,float a1,float a2) { if(a0!=1.25f || a1!=2.25f || a2!=3.25f) failure=0x502; }
void glMap1f(unsigned target,float u1,float u2,int us,int uo,const float* p) {
 static const unsigned sizes[]={4,1,3,1,2,3,4,3,4};
 if(uo<=0 || u1==u2) { failure=0x501; return; }
 if(target<0xd90 || target>=0xd90+9 || u1!=-1.25 || u2!=2.5 || us!=19 || uo!=1024 || (uintptr_t)p<=UINT32_MAX) { failure=0x502; return; }
 unsigned n=sizes[target-0xd90];
 for(int u=0;u<uo;u++) for(int v=0;v<1;v++) for(unsigned c=0;c<n;c++) {
 size_t i=(size_t)u*us+0+c;
 if(p[i]!=(float)(i+0.125)) failure=0x502;
 }
}
void glMap2f(unsigned target,float u1,float u2,int us,int uo,float v1,float v2,int vs,int vo,const float* p) {
 static const unsigned sizes[]={4,1,3,1,2,3,4,3,4};
 if(uo<=0 || u1==u2) { failure=0x501; return; }
 if(target<0xdb0 || target>=0xdb0+9 || u1!=-1.25 || u2!=2.5 || us!=19 || uo!=1024 || v1!=-3.5 || v2!=4.75 || vs!=5 || vo!=7 || (uintptr_t)p<=UINT32_MAX) { failure=0x502; return; }
 unsigned n=sizes[target-0xdb0];
 for(int u=0;u<uo;u++) for(int v=0;v<vo;v++) for(unsigned c=0;c<n;c++) {
 size_t i=(size_t)u*us+(size_t)v*vs+c;
 if(p[i]!=(float)(i+0.125)) failure=0x502;
 }
}
void glMap1d(unsigned target,double u1,double u2,int us,int uo,const double* p) {
 static const unsigned sizes[]={4,1,3,1,2,3,4,3,4};
 if(uo<=0 || u1==u2) { failure=0x501; return; }
 if(target<0xd90 || target>=0xd90+9 || u1!=-1.25 || u2!=2.5 || us!=19 || uo!=1024 || (uintptr_t)p<=UINT32_MAX) { failure=0x502; return; }
 unsigned n=sizes[target-0xd90];
 for(int u=0;u<uo;u++) for(int v=0;v<1;v++) for(unsigned c=0;c<n;c++) {
 size_t i=(size_t)u*us+0+c;
 if(p[i]!=(double)(i+0.125)) failure=0x502;
 }
}
void glMap2d(unsigned target,double u1,double u2,int us,int uo,double v1,double v2,int vs,int vo,const double* p) {
 static const unsigned sizes[]={4,1,3,1,2,3,4,3,4};
 if(uo<=0 || u1==u2) { failure=0x501; return; }
 if(target<0xdb0 || target>=0xdb0+9 || u1!=-1.25 || u2!=2.5 || us!=19 || uo!=1024 || v1!=-3.5 || v2!=4.75 || vs!=5 || vo!=7 || (uintptr_t)p<=UINT32_MAX) { failure=0x502; return; }
 unsigned n=sizes[target-0xdb0];
 for(int u=0;u<uo;u++) for(int v=0;v<vo;v++) for(unsigned c=0;c<n;c++) {
 size_t i=(size_t)u*us+(size_t)v*vs+c;
 if(p[i]!=(double)(i+0.125)) failure=0x502;
 }
}

void glGenProgramsARB(int n,unsigned *p) { if(n!=1024 || (uintptr_t)p<=UINT32_MAX) {failure=0x502;return;} for(int i=0;i<n;i++) p[i]=0x1200u+i; }
static void vector(unsigned target,unsigned index,const float *p) { if(target!=0x8620 || index!=3 || (uintptr_t)p<=UINT32_MAX) {failure=0x502;return;} for(int i=0;i<4;i++) if(p[i]!=i+0.25f) failure=0x502; }
void glProgramEnvParameter4fvARB(unsigned t,unsigned i,const float *p) {vector(t,i,p);}
void glProgramLocalParameter4fvARB(unsigned t,unsigned i,const float *p) {vector(t,i,p);}
static void data_check(const void *ptr) { if((uintptr_t)ptr<=UINT32_MAX) {failure=0x502;return;} const unsigned char *p=ptr; for(int i=0;i<768;i++) if(p[i]!=(unsigned char)i) failure=0x502; }
void glProgramStringARB(unsigned t,unsigned f,int n,const void *p) {if(t!=0x8620 || f!=0x8875 || n!=768) failure=0x502; else data_check(p);}
void glColorTableEXT(unsigned t,unsigned internal,int n,unsigned f,unsigned type,const void *p) {if(t!=0x81fb || internal!=0x1907 || n!=256 || f!=0x1907 || type!=0x1401) failure=0x502; else data_check(p);}
