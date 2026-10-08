#pragma once
#include "yggdrasil/node.hpp"
#include "bifrost/binder_abi.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
namespace arm64emu::yggdrasil {
class BinderNode;
struct BinderObject {
    std::weak_ptr<Node> owner;
    uint64_t pointer = 0, cookie = 0;
    uint32_t strong = 0, weak = 0;
    bool alive = true;
    bool accepts_fds = false;
    uint32_t pending_strong = 0, pending_weak = 0;
};
struct BinderService {
    std::shared_ptr<BinderObject> object;
    uint32_t dump_priority;
    int32_t stability;
};
// One domain per device per emulator. Never connects to host Binder services.
struct BinderDomain {
    // Weak peer promotion can release the last Node on scope exit; allow
    // its destructor to clean up under the same domain lock.
    std::recursive_mutex mutex;
    std::condition_variable_any ready;
    BinderNode* manager = nullptr; // protected by mutex; destructor removes it
    uint64_t manager_ptr = 0, manager_cookie = 0;
    bool stopped = false;
    bool service_manager = false;
    bool manager_accepts_fds = true;
    std::map<std::string, BinderService> services;
};
class BinderNode final : public Node {
public:
    BinderNode(std::shared_ptr<BinderDomain> domain, int flags);
    ~BinderNode() override;
    ssize_t read(uint64_t, void*, size_t) override { return -EINVAL; }
    ssize_t write(uint64_t, const void*, size_t) override { return -EINVAL; }
    ssize_t lseek(int64_t, int) override { return -ESPIPE; }
    int fstat(struct stat*) override;
    bool seekable() const override { return false; }
    int flags() const override { return flags_.load(); }
    void set_flags(int flags) override;
    int ioctl_thread(uint32_t, uint64_t, Memory&, CPU&, FdTable&) override;
    int64_t mmap_guest(Memory&, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) override;
    bool owns_guest_mappings() const override { return true; }
    void invalidate_mapping(uint64_t, uint64_t) override;
    int protect_mapping(uint64_t, uint64_t, uint64_t) override;
private:
    using Object = BinderObject;
    struct Handle {
        std::shared_ptr<Object> object;
        uint32_t strong = 0, weak = 0, temporary_strong = 0, temporary_weak = 0;
    };
    struct Reference { uint32_t handle; bool weak; std::shared_ptr<Object> object; };
    // Hold the open file until delivery, even if the sender closes its fd.
    // Receiver fd numbers are allocated only when BR_TRANSACTION/REPLY is read.
    struct Descriptor { uint64_t offset; std::shared_ptr<Node> node; };
    struct Event {
        explicit Event(uint32_t value = 0) : command(value) {}
        uint32_t command;
        bf_binder_transaction transaction{};
        std::vector<uint8_t> payload;
        std::vector<Reference> references;
        std::vector<Descriptor> descriptors;
        bf_binder_ptr_cookie object{};
        std::weak_ptr<Node> caller;
        int caller_tid = 0;
    };
    struct Call { std::weak_ptr<Node> caller; int tid; bool accepts_fds = false; };
    std::shared_ptr<BinderDomain> domain_;
    std::atomic<int> flags_;
    uint32_t max_threads_ = 15;
    uint64_t mapping_ = 0, mapping_size_ = 0;
    uint64_t queued_bytes_ = 0;
    Memory* memory_ = nullptr;
    uint8_t* driver_mapping_ = nullptr;
    uint64_t driver_size_ = 0;
    std::map<uint64_t, uint64_t> free_;
    std::map<uint64_t, uint64_t> buffers_;
    std::map<uint64_t, std::vector<Reference>> buffer_references_;
    std::map<uint64_t, std::shared_ptr<Object>> owned_objects_;
    std::map<uint32_t, Handle> handles_;
    uint32_t next_handle_ = 1;
    std::map<int, std::deque<Event>> replies_;
    std::deque<Event> incoming_;
    std::map<int, std::vector<Call>> calls_;
    std::set<int> loopers_;
    std::map<int, bf_binder_error> errors_;
    int write_read(uint64_t, Memory&, CPU&, FdTable&, std::unique_lock<std::recursive_mutex>&);
    int command(uint32_t, const uint8_t*, Memory&, int, FdTable&);
    uint64_t allocate_buffer(Memory&, const std::vector<uint8_t>&);
    int free_buffer(Memory&, uint64_t);
    void fail(int tid, uint32_t command, int error);
    int translate_objects(BinderNode&, Event&, Memory&, const bf_binder_transaction&, int tid,
                          FdTable&, bool accepts_fds = false);
    int reference_command(uint32_t, const uint8_t*);
    void change_reference(const std::shared_ptr<Object>&, int strong, int weak, int owner_tid = 0);
    void drop_buffer_references(uint64_t);
    void release_all_references();
    int service_transaction(const bf_binder_transaction&, Memory&, int tid, FdTable&);
};
}
