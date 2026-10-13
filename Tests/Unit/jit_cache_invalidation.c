/* A worker keeps a compiled target hot. Synchronized IC maintenance must
 * revoke that CPU's cached translation when another CPU rewrites the target. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static unsigned request,done;
static int failed;
static unsigned first_bad_round;
static uint64_t first_bad_value;
static uint32_t *code;
static int data_mapping_churn(void) {
 const unsigned count=32768;
 const size_t length=count*8;
 uint32_t *functions=mmap(NULL,length,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 void *data=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(functions==MAP_FAILED || data==MAP_FAILED)return 2;
 for(unsigned i=0;i<count;i++) {
  functions[2*i]=0xd2800020;functions[2*i+1]=0xd65f03c0;
 }
 __builtin___clear_cache((char*)functions,(char*)functions+length);
 for(unsigned i=0;i<count;i++) {
  uint64_t (*fn)(void)=(void*)(functions+2*i);
  if(fn()!=1)return 3;
 }
 struct timespec begin,end;
 clock_gettime(CLOCK_MONOTONIC,&begin);
 for(unsigned i=0;i<4096;i++)
  if(mprotect(data,4096,(i&1)?PROT_READ|PROT_WRITE:PROT_READ))return 4;
 clock_gettime(CLOCK_MONOTONIC,&end);
 printf("data-only invalidation churn: %.6f seconds\n",
        end.tv_sec-begin.tv_sec+(end.tv_nsec-begin.tv_nsec)*1e-9);
 // The fast negative lookup must not suppress subsequent code invalidation.
 functions[2*(count-1)]=0xd2800540; // mov x0,#42
 __builtin___clear_cache((char*)(functions+2*(count-1)),(char*)(functions+2*count));
 uint64_t (*fn)(void)=(void*)(functions+2*(count-1));
 if(fn()!=42)return 5;
 // Guest JITs publish code with IC IVAU per line on pages holding live code.
 // Each rewrite must take effect without scanning every cached block.
 uint32_t *patched=functions+2*(count/2);
 uint64_t (*patched_fn)(void)=(void*)patched;
 clock_gettime(CLOCK_MONOTONIC,&begin);
 for(unsigned i=0;i<4096;i++) {
  patched[0]=0xd2800000|((i&0xffff)<<5); // mov x0,#i
  __builtin___clear_cache((char*)patched,(char*)(patched+2));
  if(patched_fn()!=i)return 7;
 }
 clock_gettime(CLOCK_MONOTONIC,&end);
 printf("code-page invalidation churn: %.6f seconds\n",
        end.tv_sec-begin.tv_sec+(end.tv_nsec-begin.tv_nsec)*1e-9);
 // Same-page neighbours were retired with the page; they must retranslate.
 for(unsigned i=count/2-4;i<count/2+4;i++) {
  uint64_t (*neighbour)(void)=(void*)(functions+2*i);
  if(i!=count/2 && neighbour()!=1)return 8;
 }
 if(munmap(data,4096) || munmap(functions,length))return 6;
 puts("jit_cache_invalidation: ALL PASS");return 0;
}
// Single-threaded: a caller on page 0 BLs a callee on page 1, so the JIT
// patches a direct call into the callee's translation. Rewriting only the
// callee (cache maintenance by line, or mprotect without maintenance) must
// reach the surviving caller; unchanged code and neighbours keep working.
static int retained_callers(void) {
 uint32_t *page=mmap(NULL,8192,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(page==MAP_FAILED)return 2;
 // same_line shares the callee's 64-byte cache line; neighbour does not.
 uint32_t *caller=page,*callee=page+1024,*same_line=callee+2,*neighbour=page+1024+16;
 caller[0]=0xa9bf7bfd;                     // stp x29,x30,[sp,#-16]!
 caller[1]=0x94000000|(uint32_t)(callee-(caller+1)); // bl callee
 caller[2]=0xa8c17bfd;                     // ldp x29,x30,[sp],#16
 caller[3]=0xd65f03c0;                     // ret
 callee[0]=0xd2800020;callee[1]=0xd65f03c0; // mov x0,#1; ret
 neighbour[0]=0xd2800120;neighbour[1]=0xd65f03c0; // mov x0,#9; ret
 same_line[0]=0xd28000a0;same_line[1]=0xd65f03c0; // mov x0,#5; ret
 __builtin___clear_cache((char*)page,(char*)page+8192);
 uint64_t (*call)(void)=(void*)caller,(*near)(void)=(void*)neighbour,(*line)(void)=(void*)same_line;
 for(int i=0;i<1000;i++) if(call()!=1 || near()!=9 || line()!=5)return 3;
 callee[0]=0xd2800040;                     // mov x0,#2
 __builtin___clear_cache((char*)callee,(char*)(callee+2));
 for(int i=0;i<1000;i++) if(call()!=2)return 4;
 if(near()!=9 || line()!=5)return 5;
 if(mprotect(page+1024,4096,PROT_READ|PROT_WRITE))return 6;
 callee[0]=0xd2800060;                     // mov x0,#3, published by mprotect
 if(mprotect(page+1024,4096,PROT_READ|PROT_EXEC))return 6;
 for(int i=0;i<1000;i++) if(call()!=3)return 7;
 if(mprotect(page,4096,PROT_READ|PROT_EXEC))return 6; // unchanged bytes
 for(int i=0;i<1000;i++) if(call()!=3 || near()!=9)return 8;
 if(munmap(page,8192))return 6;
 puts("jit_cache_invalidation: ALL PASS");return 0;
}
static void *worker(void *arg) {
 (void)arg;
 uint64_t (*fn)(void)=(void*)code;
 for(unsigned round=1;round<=100;round++) {
  pthread_mutex_lock(&mu);
  while(request!=round) pthread_cond_wait(&cv,&mu);
  pthread_mutex_unlock(&mu);
  for(int i=0;i<100;i++) {
   uint64_t value=fn();
   if(value!=round && !failed) {
    failed=1; first_bad_round=round; first_bad_value=value;
   }
  }
  pthread_mutex_lock(&mu);done=round;pthread_cond_signal(&cv);pthread_mutex_unlock(&mu);
 }
 return NULL;
}
int main(int argc, char **argv) {
 if(argc>1 && strcmp(argv[1],"churn")==0)return data_mapping_churn();
 if(argc>1 && strcmp(argv[1],"retain")==0)return retained_callers();
 code=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(code==MAP_FAILED)return 2;
 code[0]=0xd2800020;code[1]=0xd65f03c0;
 __builtin___clear_cache((char*)code,(char*)(code+2));
 if(argc>1 && strcmp(argv[1],"single")==0) {
  uint64_t (*fn)(void)=(void*)code;
  for(unsigned round=1;round<=100;round++) {
   code[0]=0xd2800000|(round<<5);
   __builtin___clear_cache((char*)code,(char*)(code+2));
   for(unsigned i=0;i<100;i++) if(fn()!=round)return 1;
  }
  puts("jit_cache_invalidation: ALL PASS");return 0;
 }
 pthread_t t;if(pthread_create(&t,NULL,worker,NULL))return 3;
 for(unsigned round=1;round<=100;round++) {
  pthread_mutex_lock(&mu);
  code[0]=0xd2800000|(round<<5);
  __builtin___clear_cache((char*)code,(char*)(code+2));
  request=round;pthread_cond_signal(&cv);
  while(done!=round)pthread_cond_wait(&cv,&mu);
  pthread_mutex_unlock(&mu);
 }
 pthread_join(t,NULL);
 if(failed) {
  printf("FAIL cached instruction: round=%u got=%llu\n",first_bad_round,
         (unsigned long long)first_bad_value);
  return 1;
 }
 puts("jit_cache_invalidation: ALL PASS");return 0;
}
