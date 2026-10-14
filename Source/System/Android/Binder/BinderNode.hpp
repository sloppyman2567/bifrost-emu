#pragma once
#include "System/VFS/Nodes/Node.hpp"
#include "Bifrost/BinderABI.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <functional>
namespace arm64emu::yggdrasil {
class BinderNode;
struct BinderDeath {
    std::weak_ptr<Node> recipient;
    uint64_t cookie = 0;
    bool queued = false, delivered = false, acknowledged = false, clearing = false, cancelled = false;
};
struct BinderObject {
    enum class HostService { None, GraphicsAllocator, ProcessInfo } host_service = HostService::None;
    std::weak_ptr<Node> owner;
    uint64_t pointer = 0, cookie = 0;
    uint32_t strong = 0, weak = 0;
    bool alive = true;
    bool accepts_fds = false;
    bool async_active = false; // one-way execution is serialized per Binder object
    bool reachable() const { return alive && (host_service != HostService::None || !owner.expired()); }
    std::vector<std::weak_ptr<BinderDeath>> deaths;
    uint32_t pending_strong = 0, pending_weak = 0;
};
struct BinderService {
    std::shared_ptr<BinderObject> object;
    uint32_t dump_priority;
    int32_t stability;
};
// One domain per device per emulator. Never connects to host Binder services.
struct BinderDomain {
    std::mutex mutex;
    std::set<BinderNode*> endpoints;
    void stop();
    // Keep promoted peers alive until after unlocking. Their destructors may
    // acquire this domain again; metadata locking itself is never recursive.
    struct Guard {
        BinderDomain& domain;
        Guard* previous;
        std::unique_lock<std::mutex> lock;
        std::vector<std::shared_ptr<Node>> peers;
        inline static thread_local Guard* current = nullptr;
        explicit Guard(BinderDomain& d) : domain(d), previous(current), lock(d.mutex) { current = this; }
        ~Guard() { current = previous; lock.unlock(); }
        void release_peers() {
            std::vector<std::shared_ptr<Node>> released;
            released.swap(peers);
            lock.unlock(); released.clear(); lock.lock();
        }
    };
    std::shared_ptr<Node> promote(const std::weak_ptr<Node>& weak) {
        auto peer = weak.lock();
        if (peer && (Guard::current->peers.empty() || Guard::current->peers.back() != peer))
            Guard::current->peers.push_back(peer);
        return peer;
    }
    BinderNode* manager = nullptr; // protected by mutex; destructor removes it
    uint64_t manager_ptr = 0, manager_cookie = 0;
    bool stopped = false;
    bool service_manager = false;
    bool manager_accepts_fds = true;
    uint32_t sender_euid = 0; // single guest process, not the host's credentials
    std::map<std::string, BinderService> services;
    std::set<std::string> declared_services;
    struct ProcessState { uint32_t uid; int32_t state, adjustment; };
    std::map<int, ProcessState> processes;
    int report_process_state(int pid, int32_t state);
};
class BinderNode final : public Node {
public:
    BinderNode(std::shared_ptr<BinderDomain> domain, int flags);
    ~BinderNode() override;
    void disconnect();
    void set_sender(uint32_t uid, int pid) { sender_uid_ = uid; sender_pid_ = pid; }
    int sender_pid() const { return sender_pid_; }
    int arena_fd() const { return arena_fd_; }
    int64_t map_broker_arena(Memory&, uint64_t, uint64_t);
    int ioctl_broker(uint32_t, uint64_t, Memory&, CPU&, FdTable&, const std::function<bool()>&, FdTable* source_files = nullptr);
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
    friend struct BinderDomain;
    std::condition_variable ready_;
    using Object = BinderObject;
    struct Handle {
        std::shared_ptr<Object> object;
        uint32_t strong = 0, weak = 0, temporary_strong = 0, temporary_weak = 0;
        std::shared_ptr<BinderDeath> death;
    };
    struct Reference { uint32_t handle; bool weak; std::shared_ptr<Object> object; };
    // Hold the open file until delivery, even if the sender closes its fd.
    // Receiver fd numbers are allocated only when BR_TRANSACTION/REPLY is read.
    struct Descriptor { uint64_t offset; std::shared_ptr<Node> node; uint32_t width = 8; bool auto_close = false; };
    struct Fixup { uint64_t offset, target; };
    struct Event {
        explicit Event(uint32_t value = 0) : command(value) {}
        uint32_t command;
        bf_binder_transaction transaction{};
        std::vector<uint8_t> payload;
        std::vector<Reference> references;
        std::vector<Descriptor> descriptors;
        std::vector<Fixup> fixups;
        std::shared_ptr<BinderDeath> death;
        bf_binder_ptr_cookie object{};
        std::weak_ptr<Node> caller;
        int caller_tid = 0;
        std::shared_ptr<Object> async_object;
    };
    struct TransactionInput { std::vector<uint8_t> payload; std::vector<uint64_t> offsets; int error = 0; };
    TransactionInput prepare_transaction(const uint8_t*, bool scatter, Memory&);
    struct Call { std::weak_ptr<Node> caller; int tid; bool accepts_fds = false; };
    std::shared_ptr<BinderDomain> domain_;
    std::atomic<int> flags_;
    uint32_t max_threads_ = 15;
    uint64_t mapping_ = 0, mapping_size_ = 0;
    uint64_t queued_bytes_ = 0;
    Memory* memory_ = nullptr;
    int arena_fd_ = -1;
    uint32_t sender_uid_ = 0;
    int sender_pid_ = 1;
    bool disconnected_ = false;
    int64_t map_arena(Memory&, uint64_t, uint64_t, bool);
    uint8_t* driver_mapping_ = nullptr;
    uint64_t driver_size_ = 0;
    std::map<uint64_t, uint64_t> free_;
    std::map<uint64_t, uint64_t> buffers_;
    std::map<uint64_t, std::vector<Reference>> buffer_references_;
    std::map<uint64_t, std::shared_ptr<Object>> async_buffers_;
    struct AutoFd { FdTable* table; int number; std::weak_ptr<Node> file; };
    std::map<uint64_t, std::vector<AutoFd>> buffer_fds_;
    std::vector<std::shared_ptr<BinderDeath>> deaths_;
    std::map<uint64_t, std::shared_ptr<Object>> owned_objects_;
    std::map<uint32_t, Handle> handles_;
    uint32_t next_handle_ = 1;
    std::map<int, std::deque<Event>> replies_;
    std::deque<Event> incoming_;
    std::map<int, std::vector<Call>> calls_;
    std::set<int> loopers_, registered_loopers_, waiting_loopers_;
    uint32_t requested_threads_ = 0;
    std::map<int, bf_binder_error> errors_;
    int write_read(uint64_t, Memory&, CPU&, FdTable&, std::unique_lock<std::mutex>&, const std::function<bool()>&, FdTable*);
    int command(uint32_t, const uint8_t*, Memory&, int, FdTable&, TransactionInput*);
    uint64_t allocate_buffer(Memory&, const std::vector<uint8_t>&);
    int free_buffer(Memory&, uint64_t);
    void fail(int tid, uint32_t command, int error);
    int translate_objects(BinderNode&, Event&, Memory&, const bf_binder_transaction&, int tid,
                          FdTable&, bool accepts_fds = false, uint64_t buffers_size = 0, const TransactionInput* input = nullptr);
    int death_command(uint32_t, const uint8_t*, int);
    void prune_death(Handle&);
    void notify_death(const std::shared_ptr<BinderDeath>&);
    void close_buffer_fds(uint64_t);
    int reference_command(uint32_t, const uint8_t*);
    void change_reference(const std::shared_ptr<Object>&, int strong, int weak, int owner_tid = 0);
    void drop_buffer_references(uint64_t);
    void release_all_references();
    int service_transaction(const bf_binder_transaction&, Memory&, int tid, FdTable&);
    int allocator_transaction(const bf_binder_transaction&, Memory&, int tid);
    int process_info_transaction(const bf_binder_transaction&, Memory&, int tid);
};
}
