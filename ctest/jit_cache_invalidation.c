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
 if(munmap(data,4096) || munmap(functions,length))return 6;
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
