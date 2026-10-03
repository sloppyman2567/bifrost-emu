/* Host probes intentionally avoid a GPU/context. They reject raw guest
 * data addresses rather than accidentally dereferencing them on the host. */
#include <stdint.h>
#include <stddef.h>
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
unsigned glGetError(void) { unsigned e=last_error; last_error=0; return e; }
#endif
#if defined(THUNK_VK_PROBE)
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
