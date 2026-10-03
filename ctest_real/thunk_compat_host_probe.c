/* Host probes intentionally avoid a GPU/context. They reject raw guest
 * data addresses rather than accidentally dereferencing them on the host. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(THUNK_GL_PROBE)
static unsigned array_buffer, last_error;
void glBindBuffer(unsigned target,unsigned buffer) {
    if(target==0x8892) array_buffer=buffer;
}
static void check_pointer(const void *p) {
    if(array_buffer) { if((uintptr_t)p!=16) last_error=0x502; return; }
    if((uintptr_t)p<=UINT32_MAX) { last_error=0x502; return; }
    const int *data=p;
    for(int i=0;i<4;i++) if(data[i]!=(i+1)*10) last_error=0x502;
}
void glVertexAttribIPointer(unsigned index,int size,unsigned type,int stride,const void *p) {
    (void)index; (void)size; (void)type; (void)stride; check_pointer(p);
}
void glVertexAttribPointer(unsigned index,int size,unsigned type,unsigned char normalized,int stride,const void *p) {
    (void)index; (void)size; (void)type; (void)normalized; (void)stride; check_pointer(p);
}
void glMap1d(unsigned target,double u1,double u2,int stride,int order,const double *points) {
    static const unsigned components[9]={4,1,3,1,2,3,4,3,4};
    if(target<0xd90 || target>0xd98) { last_error=0x500; return; }
    unsigned n=components[target-0xd90];
    if(stride<(int)n || order<=0 || u1==u2) { last_error=0x501; return; }
    if(u1!=-1.25 || u2!=2.5 || (uintptr_t)points<=UINT32_MAX) { last_error=0x502; return; }
    for(int point=0;point<order;point++)
        for(unsigned c=0;c<n;c++) {
            size_t i=(size_t)point*stride+c;
            if(points[i]!=(double)i+0.125) last_error=0x502;
        }
}
unsigned glGetError(void) { unsigned e=last_error; last_error=0; return e; }
#endif
#if defined(THUNK_VK_PROBE)
static unsigned char mapped_memory[8192];
static uint64_t mapped_offset;
int vkAllocateMemory(uint64_t device,const void *info,const void *allocator,uint64_t *memory) {
    (void)device; (void)info; (void)allocator;
    memset(mapped_memory,0,sizeof(mapped_memory)); *memory=0x55; return 0;
}
int vkMapMemory(uint64_t device,uint64_t memory,uint64_t offset,uint64_t size,uint32_t flags,void **data) {
    (void)device; (void)flags;
    if(memory!=0x55 || offset>sizeof(mapped_memory) || size>sizeof(mapped_memory)-offset) return -5;
    mapped_offset=offset; *data=mapped_memory+offset; return 0;
}
void vkUnmapMemory(uint64_t device,uint64_t memory) {(void)device;(void)memory;}
void vkFreeMemory(uint64_t device,uint64_t memory,const void *allocator) {(void)device;(void)memory;(void)allocator;}
int vkQueueSubmit(uint64_t queue,uint32_t count,const void *submits,uint64_t fence) {
    (void)queue;(void)count;(void)submits;(void)fence;
    return mapped_memory[mapped_offset+3]==0x31 && mapped_memory[mapped_offset+7]==0x52 &&
           mapped_memory[mapped_offset+128]==0xd7 ? 0 : -3;
}
int vkWaitForFences(uint64_t device,uint32_t count,const uint64_t *fences,uint32_t all,uint64_t timeout) {
    (void)device;(void)count;(void)fences;(void)all;(void)timeout;
    mapped_memory[mapped_offset+128]=0xd7; mapped_memory[mapped_offset+4096]=0xb4; return 0;
}
int vkFlushMappedMemoryRanges(uint64_t device,uint32_t count,const void *ranges) {
    (void)device;(void)count;(void)ranges; return 0;
}
int vkInvalidateMappedMemoryRanges(uint64_t device,uint32_t count,const void *ranges) {
    (void)device;(void)count;(void)ranges;
    mapped_memory[mapped_offset+256]=0xab; return 0;
}
typedef struct Node { uint32_t type; const struct Node *next; uint32_t count; const uint64_t *devices; } Node;
int vkCreateDevice(uint64_t physical,const void *info,const void *allocator,uint64_t *device) {
    (void)allocator;
    if(physical!=0x1234 || (uintptr_t)info<=UINT32_MAX || (uintptr_t)device<=UINT32_MAX) return -3;
    const Node *node=*(const Node * const*)((const unsigned char*)info+8);
    int links=0;
    while(node) {
        if(++links>8 || (uintptr_t)node<=UINT32_MAX || node->type!=1000070001 ||
           (node->count!=1 && node->count!=1024) || (uintptr_t)node->devices<=UINT32_MAX) return -3;
        for(uint32_t i=0;i<node->count;i++) if(node->devices[i]!=0x1234+i) return -3;
        node=node->next;
    }
    if(links!=1 && links!=8) return -3;
    *device=0x12345678; return 0;
}
int vkQueuePresentKHR(uint64_t queue,const void *info) {
    if(queue!=0x1234 || (uintptr_t)info<=UINT32_MAX) return -3;
    const unsigned char *group=*(const unsigned char * const*)((const unsigned char*)info+8);
    if((uintptr_t)group<=UINT32_MAX || *(const uint32_t*)group!=1000060011 ||
       *(const uint32_t*)(group+16)!=1) return -3;
    const uint32_t *masks=*(const uint32_t * const*)(group+24);
    if((uintptr_t)masks<=UINT32_MAX || *masks!=1) return -3;
    return 0;
}
#endif
