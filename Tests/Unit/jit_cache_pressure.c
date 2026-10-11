/* Generate many distinct functions, then call them repeatedly from threads.
 * Each function returns seed+48; loads/stores prevent constant folding away
 * the block body. Cached failures must remain correct under cache pressure. */
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define NFUN 4000
#define WORDS 100
extern uint64_t interp_leaf(uint64_t);
__asm__(".text\n .align 2\n .global interp_leaf\n interp_leaf:\n"
        ".rept 8\n frintm v0.2s,v0.2s\n .endr\n add x0,x0,#1\n ret\n");
static uint32_t *code;
static int failed;
static void *run(void *arg) {
 uintptr_t seed=(uintptr_t)arg;
 for(unsigned i=0;i<300;i++) if(interp_leaf(seed+i)!=seed+i+1) {
  __atomic_store_n(&failed,1,__ATOMIC_RELAXED);return NULL;
 }
 for(unsigned pass=0;pass<3;pass++) for(unsigned i=0;i<NFUN;i++) {
  uint64_t (*fn)(uint64_t, uint64_t*)=(void*)(code+i*WORDS);
  uint64_t value=seed+i;
  uint64_t got=fn(seed+i,&value);
  if(got!=seed+i+48 || value!=got) {__atomic_store_n(&failed,1,__ATOMIC_RELAXED);return NULL;}
 }
 return NULL;
}
int main(int argc, char **argv) {
 code=mmap(NULL,NFUN*WORDS*4,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(code==MAP_FAILED) return 2;
 for(unsigned i=0;i<NFUN;i++) {
  uint32_t *p=code+i*WORDS;
  for(unsigned j=0;j<48;j++) {p[j*2]=0x91000400; /* add x0,x0,#1 */ p[j*2+1]=0xf9000020; /* str x0,[x1] */}
  p[96]=0xd65f03c0; /* ret */
 }
 __builtin___clear_cache((char*)code,(char*)(code+NFUN*WORDS));
 if(argc>1 && strcmp(argv[1],"single")==0) {
  run((void*)20000);
  if(failed)return 1;
  puts("jit_cache_pressure: ALL PASS");return 0;
 }
 pthread_t t[2];
 for(uintptr_t i=0;i<2;i++) if(pthread_create(t+i,NULL,run,(void*)(10000*i))) return 3;
 run((void*)20000);
 for(int i=0;i<2;i++) pthread_join(t[i],NULL);
 if(failed) return 1;
 puts("jit_cache_pressure: ALL PASS");return 0;
}
