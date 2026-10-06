/* AArch64 Binder protocol 8 layout. Linux UAPI binder.h (v6.12).
 * Keep pointer-sized fields explicit: guest pointers are not host pointers. */
#ifndef BIFROST_BINDER_ABI_H
#define BIFROST_BINDER_ABI_H
#include <stdint.h>
#define BF_IOC(dir,type,nr,size) (((uint32_t)(dir)<<30)|((uint32_t)(size)<<16)|((uint32_t)(type)<<8)|(uint32_t)(nr))
#define BF_IO(type,nr) BF_IOC(0,type,nr,0)
#define BF_IOR(type,nr,T) BF_IOC(2,type,nr,sizeof(T))
#define BF_IOW(type,nr,T) BF_IOC(1,type,nr,sizeof(T))
#define BF_IOWR(type,nr,T) BF_IOC(3,type,nr,sizeof(T))
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
#define BF_BINDER_WRITE_READ BF_IOWR('b',1,struct bf_binder_write_read)
#define BF_BINDER_SET_MAX_THREADS BF_IOW('b',5,uint32_t)
#define BF_BINDER_SET_CONTEXT_MGR BF_IOW('b',7,int32_t)
#define BF_BINDER_THREAD_EXIT BF_IOW('b',8,int32_t)
#define BF_BINDER_VERSION BF_IOWR('b',9,int32_t)
#define BF_BINDER_SET_CONTEXT_MGR_EXT BF_IOW('b',13,struct bf_flat_binder)
#define BF_BINDER_GET_EXTENDED_ERROR BF_IOWR('b',17,struct bf_binder_error)
#define BF_BC_TRANSACTION BF_IOW('c',0,struct bf_binder_transaction)
#define BF_BC_REPLY BF_IOW('c',1,struct bf_binder_transaction)
#define BF_BC_FREE_BUFFER BF_IOW('c',3,uint64_t)
#define BF_BC_INCREFS BF_IOW('c',4,uint32_t)
#define BF_BC_ACQUIRE BF_IOW('c',5,uint32_t)
#define BF_BC_RELEASE BF_IOW('c',6,uint32_t)
#define BF_BC_DECREFS BF_IOW('c',7,uint32_t)
#define BF_BC_INCREFS_DONE BF_IOW('c',8,struct bf_binder_ptr_cookie)
#define BF_BC_ACQUIRE_DONE BF_IOW('c',9,struct bf_binder_ptr_cookie)
#define BF_BC_REGISTER_LOOPER BF_IO('c',11)
#define BF_BC_ENTER_LOOPER BF_IO('c',12)
#define BF_BC_EXIT_LOOPER BF_IO('c',13)
#define BF_BR_OK BF_IO('r',1)
#define BF_BR_TRANSACTION BF_IOR('r',2,struct bf_binder_transaction)
#define BF_BR_REPLY BF_IOR('r',3,struct bf_binder_transaction)
#define BF_BR_DEAD_REPLY BF_IO('r',5)
#define BF_BR_TRANSACTION_COMPLETE BF_IO('r',6)
#define BF_BR_INCREFS BF_IOR('r',7,struct bf_binder_ptr_cookie)
#define BF_BR_ACQUIRE BF_IOR('r',8,struct bf_binder_ptr_cookie)
#define BF_BR_RELEASE BF_IOR('r',9,struct bf_binder_ptr_cookie)
#define BF_BR_DECREFS BF_IOR('r',10,struct bf_binder_ptr_cookie)
#define BF_BR_NOOP BF_IO('r',12)
#define BF_BR_FAILED_REPLY BF_IO('r',17)
#define BF_TF_ONE_WAY 1u
/* Android IBinder.FLAG_COLLECT_NOTED_APP_OPS is passed through to Binder. */
#define BF_TF_COLLECT_NOTED_APP_OPS 2u
#define BF_TF_CLEAR_BUF 0x20u
#define BF_BINDER_TYPE_BINDER 0x73622a85u
#define BF_BINDER_TYPE_WEAK_BINDER 0x77622a85u
#define BF_BINDER_TYPE_HANDLE 0x73682a85u
#define BF_BINDER_TYPE_WEAK_HANDLE 0x77682a85u
#endif
