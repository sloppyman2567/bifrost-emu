/* AArch64 Binder protocol 8 layout. Linux UAPI binder.h (v6.12).
 * Keep pointer-sized fields explicit: guest pointers are not host pointers. */
#ifndef BIFROST_BINDER_ABI_H
#define BIFROST_BINDER_ABI_H
#include <stdint.h>
#include <sys/ioctl.h>
struct bf_binder_write_read {
    uint64_t write_size, write_consumed, write_buffer;
    uint64_t read_size, read_consumed, read_buffer;
};
struct bf_binder_transaction {
    uint64_t target, cookie;
    uint32_t code, flags;
    int32_t sender_pid;
    uint32_t sender_euid;
    uint64_t data_size, offsets_size, buffer, offsets;
};
struct bf_flat_binder { uint32_t type, flags; uint64_t binder, cookie; };
struct bf_binder_ptr_cookie { uint64_t ptr, cookie; };
struct bf_binder_error { uint32_t id, command; int32_t param; };
#define BF_BINDER_WRITE_READ _IOWR('b',1,struct bf_binder_write_read)
#define BF_BINDER_SET_MAX_THREADS _IOW('b',5,uint32_t)
#define BF_BINDER_SET_CONTEXT_MGR _IOW('b',7,int32_t)
#define BF_BINDER_THREAD_EXIT _IOW('b',8,int32_t)
#define BF_BINDER_VERSION _IOWR('b',9,int32_t)
#define BF_BINDER_SET_CONTEXT_MGR_EXT _IOW('b',13,struct bf_flat_binder)
#define BF_BINDER_GET_EXTENDED_ERROR _IOWR('b',17,struct bf_binder_error)
#define BF_BC_TRANSACTION _IOW('c',0,struct bf_binder_transaction)
#define BF_BC_REPLY _IOW('c',1,struct bf_binder_transaction)
#define BF_BC_FREE_BUFFER _IOW('c',3,uint64_t)
#define BF_BC_INCREFS _IOW('c',4,uint32_t)
#define BF_BC_ACQUIRE _IOW('c',5,uint32_t)
#define BF_BC_RELEASE _IOW('c',6,uint32_t)
#define BF_BC_DECREFS _IOW('c',7,uint32_t)
#define BF_BC_INCREFS_DONE _IOW('c',8,struct bf_binder_ptr_cookie)
#define BF_BC_ACQUIRE_DONE _IOW('c',9,struct bf_binder_ptr_cookie)
#define BF_BC_REGISTER_LOOPER _IO('c',11)
#define BF_BC_ENTER_LOOPER _IO('c',12)
#define BF_BC_EXIT_LOOPER _IO('c',13)
#define BF_BR_OK _IO('r',1)
#define BF_BR_TRANSACTION _IOR('r',2,struct bf_binder_transaction)
#define BF_BR_REPLY _IOR('r',3,struct bf_binder_transaction)
#define BF_BR_DEAD_REPLY _IO('r',5)
#define BF_BR_TRANSACTION_COMPLETE _IO('r',6)
#define BF_BR_INCREFS _IOR('r',7,struct bf_binder_ptr_cookie)
#define BF_BR_ACQUIRE _IOR('r',8,struct bf_binder_ptr_cookie)
#define BF_BR_RELEASE _IOR('r',9,struct bf_binder_ptr_cookie)
#define BF_BR_DECREFS _IOR('r',10,struct bf_binder_ptr_cookie)
#define BF_BR_NOOP _IO('r',12)
#define BF_BR_FAILED_REPLY _IO('r',17)
#define BF_TF_ONE_WAY 1u
#define BF_TF_CLEAR_BUF 0x20u
#define BF_BINDER_TYPE_BINDER 0x73622a85u
#define BF_BINDER_TYPE_WEAK_BINDER 0x77622a85u
#define BF_BINDER_TYPE_HANDLE 0x73682a85u
#define BF_BINDER_TYPE_WEAK_HANDLE 0x77682a85u
#endif
