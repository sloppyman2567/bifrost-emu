#include "System/Android/Binder/BinderNode.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/VFS/VFS.hpp"
#include "System/Core/DebugFlags.hpp"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
static_assert(sizeof(bf_binder_transaction) == 64);
static_assert(sizeof(bf_binder_write_read) == 48);
static_assert(sizeof(bf_binder_handle_cookie) == 12);
static_assert(sizeof(bf_binder_transaction_sg) == 72);
static_assert(sizeof(bf_binder_buffer) == 40);
static_assert(sizeof(bf_binder_fd_array) == 32);
namespace {
constexpr size_t LIMIT = 1024 * 1024;
constexpr size_t MAX_EVENTS = 256;
bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return n && m && a < b + m && b < a + n;
}
}
BinderNode::BinderNode(std::shared_ptr<BinderDomain> domain, int flags)
    : domain_(std::move(domain)), flags_(flags) {
    std::lock_guard<std::mutex> lock(domain_->mutex);
    sender_uid_ = domain_->sender_euid;
    domain_->endpoints.insert(this);
}
void BinderDomain::stop() {
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
    for (auto* endpoint : endpoints) endpoint->ready_.notify_all();
}
BinderNode::~BinderNode() {
    disconnect();
    if (driver_mapping_) ::munmap(driver_mapping_, driver_size_);
    if (arena_fd_ >= 0) ::close(arena_fd_);
}
void BinderNode::disconnect() {
    BinderDomain::Guard guard(*domain_);
    if (disconnected_) return;
    disconnected_ = true;
    domain_->endpoints.erase(this);
    if (domain_->manager == this) domain_->manager = nullptr;
    release_all_references();
    auto abandon = [&](const Call& call) {
        if (auto node = domain_->promote(call.caller)) {
            auto caller = static_cast<BinderNode*>(node.get());
            caller->replies_[call.tid].push_back(Event{BF_BR_DEAD_REPLY});
            caller->ready_.notify_all();
        }
    };
    for (const auto& e : incoming_)
        if (!(e.transaction.flags & BF_TF_ONE_WAY)) abandon({e.caller, e.caller_tid});
    for (const auto& entry : calls_) for (const auto& call : entry.second) abandon(call);
    ready_.notify_all();
}
int BinderNode::fstat(struct stat* st) {
    memset(st, 0, sizeof(*st)); st->st_mode = S_IFCHR | 0666; st->st_nlink = 1;
    return 0;
}
void BinderNode::set_flags(int flags) {
    BinderDomain::Guard guard(*domain_);
    flags_ = (flags_ & ~O_NONBLOCK) | (flags & O_NONBLOCK);
}
void BinderNode::invalidate_mapping(uint64_t addr, uint64_t size) {
    BinderDomain::Guard guard(*domain_);
    if (overlaps(addr, size, mapping_, mapping_size_)) {
        std::vector<uint64_t> referenced;
        for (const auto& entry : buffer_references_) referenced.push_back(entry.first);
        for (uint64_t address : referenced) drop_buffer_references(address);
        while (!buffer_fds_.empty()) close_buffer_fds(buffer_fds_.begin()->first);
        mapping_ = mapping_size_ = 0; free_.clear(); buffers_.clear();
        if (driver_mapping_) ::munmap(driver_mapping_, driver_size_);
        driver_mapping_ = nullptr; driver_size_ = 0;
        if (arena_fd_ >= 0) ::close(arena_fd_);
        arena_fd_ = -1;
    }
}
int BinderNode::protect_mapping(uint64_t addr, uint64_t size, uint64_t prot) {
    BinderDomain::Guard guard(*domain_);
    return overlaps(addr, size, mapping_, mapping_size_) && (prot & 6) ? -EACCES : 0;
}
int64_t BinderNode::mmap_guest(Memory& mem, uint64_t addr, uint64_t length,
                               uint64_t prot, uint64_t flags, uint64_t offset) {
    BinderDomain::Guard guard(*domain_);
    // Binder receive arenas use MAP_PRIVATE, are read-only, offset zero, and at most 4 MiB.
    if (mapping_) return -EBUSY;
    if (!length || length > 4 * LIMIT || offset || prot != 1 ||
        (flags & 3) != 2 || (flags & (0x10 | 0x100000))) return -EINVAL;
    return map_arena(mem, addr, length, false);
}
int64_t BinderNode::map_broker_arena(Memory& mem, uint64_t addr, uint64_t length) {
    BinderDomain::Guard guard(*domain_);
    if (mapping_ || !length || length > 4 * LIMIT || (addr & Memory::PAGE_MASK)) return -EINVAL;
    return map_arena(mem, addr, length, true);
}
int64_t BinderNode::map_arena(Memory& mem, uint64_t addr, uint64_t length, bool exact) {
    uint64_t size = (length + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
    uint64_t result = mem.mmap_alloc(size, addr, false, 1, !exact);
    if (!result) return -ENOMEM;
    int fd = ::memfd_create("bifrost-binder-arena", MFD_CLOEXEC);
    if (fd < 0) { mem.untrack_allocation(result, size); return -ENOMEM; }
    void* alias = MAP_FAILED;
    if (::ftruncate(fd, size) == 0)
        alias = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED || !mem.map_device_readonly(result, size, fd)) {
        if (alias != MAP_FAILED) ::munmap(alias, size);
        ::close(fd); mem.untrack_allocation(result, size); return -ENOMEM;
    }
    arena_fd_ = fd;
    driver_mapping_ = static_cast<uint8_t*>(alias); driver_size_ = size;
    mapping_ = result; mapping_size_ = size; memory_ = &mem;
    free_[result] = size;
    return result;
}
uint64_t BinderNode::allocate_buffer(Memory& mem, const std::vector<uint8_t>& data) {
    uint64_t size = std::max<uint64_t>(8, (data.size() + 7) & ~uint64_t(7));
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        uint64_t addr = it->first, remaining = it->second - size;
        if (!mem.guest_to_host_ptr(addr, Memory::GUEST_PROT_READ, size)) return 0;
        auto host = driver_mapping_ + (addr - mapping_);
        // Driver-owned writes bypass guest WRITE permission on the receive arena.
        memset(host, 0, size);
        if (!data.empty()) memcpy(host, data.data(), data.size());
        free_.erase(it); if (remaining) free_[addr + size] = remaining;
        buffers_[addr] = size;
        return addr;
    }
    return 0;
}
int BinderNode::free_buffer(Memory& mem, uint64_t addr) {
    auto it = buffers_.find(addr);
    if (it == buffers_.end()) return -EINVAL;
    uint64_t size = it->second;
    if (!mem.guest_to_host_ptr(addr, Memory::GUEST_PROT_READ, size)) return -EFAULT;
    auto host = driver_mapping_ + (addr - mapping_);
    memset(host, 0, size); buffers_.erase(it);
    drop_buffer_references(addr);
    close_buffer_fds(addr);
    auto next = free_.lower_bound(addr);
    if (next != free_.end() && addr + size == next->first) {
        size += next->second; free_.erase(next);
    }
    next = free_.lower_bound(addr);
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr) {
            addr = prev->first; size += prev->second; free_.erase(prev);
        }
    }
    free_[addr] = size;
    return 0;
}
void BinderNode::fail(int tid, uint32_t command, int error) {
    if (dbg().binder_trace)
        fprintf(stderr, "[binder] tid=%d failure command=%#x errno=%d\n", tid, command, error);
    auto& detail = errors_[tid];
    detail = {detail.id + 1, command, -error};
    replies_[tid].push_back(Event{command});
    ready_.notify_all();
}
BinderNode::TransactionInput BinderNode::prepare_transaction(const uint8_t* bytes, bool scatter, Memory& mem) {
    TransactionInput input;
    try {
        bf_binder_transaction tx; memcpy(&tx, bytes, sizeof(tx));
        uint64_t buffers_size = 0;
        if (scatter) memcpy(&buffers_size, bytes + sizeof(tx), 8);
        uint64_t aligned = (tx.data_size + 7) & ~uint64_t(7);
        if (tx.data_size > LIMIT || tx.offsets_size > 512 || (tx.offsets_size & 7) ||
            buffers_size > LIMIT || (buffers_size & 7) || aligned + tx.offsets_size + buffers_size > LIMIT) {
            input.error = EINVAL; return input;
        }
        input.payload.resize(aligned + tx.offsets_size + buffers_size);
        if (tx.data_size) mem.read(tx.buffer, input.payload.data(), tx.data_size);
        input.offsets.resize(tx.offsets_size / 8);
        if (tx.offsets_size) {
            mem.read(tx.offsets, input.offsets.data(), tx.offsets_size);
            memcpy(input.payload.data() + aligned, input.offsets.data(), tx.offsets_size);
        }
        uint64_t used = 0, previous_end = 0;
        for (auto offset : input.offsets) {
            if ((offset & 3) || offset < previous_end || offset > tx.data_size ||
                sizeof(bf_flat_binder) > tx.data_size - offset) { input.error = EINVAL; break; }
            uint32_t type; memcpy(&type, input.payload.data() + offset, 4);
            size_t size = type == BF_BINDER_TYPE_PTR ? sizeof(bf_binder_buffer) :
                type == BF_BINDER_TYPE_FDA ? sizeof(bf_binder_fd_array) : sizeof(bf_flat_binder);
            if (size > tx.data_size - offset) { input.error = EINVAL; break; }
            previous_end = offset + size;
            if (type != BF_BINDER_TYPE_PTR) continue;
            bf_binder_buffer buffer; memcpy(&buffer, input.payload.data() + offset, sizeof(buffer));
            if (buffer.length > buffers_size - used ||
                ((buffer.length + 7) & ~uint64_t(7)) > buffers_size - used) { input.error = EINVAL; break; }
            if (buffer.length) mem.read(buffer.buffer, input.payload.data() + aligned + tx.offsets_size + used, buffer.length);
            used += (buffer.length + 7) & ~uint64_t(7);
        }
    } catch (const EmuError&) { input.error = EFAULT; }
    return input;
}
int BinderNode::command(uint32_t cmd, const uint8_t* bytes, Memory& mem, int tid, FdTable& fds, TransactionInput* input) {
    if (replies_[tid].size() >= MAX_EVENTS) return -EAGAIN;
    if (cmd == BF_BC_ENTER_LOOPER || cmd == BF_BC_REGISTER_LOOPER) {
        if (loopers_.count(tid)) return -EINVAL;
        if (cmd == BF_BC_REGISTER_LOOPER) {
            if (!requested_threads_) return -EINVAL;
            --requested_threads_; registered_loopers_.insert(tid);
        }
        loopers_.insert(tid); return 0;
    }
    if (cmd == BF_BC_EXIT_LOOPER) {
        loopers_.erase(tid); registered_loopers_.erase(tid); return 0;
    }
    if (cmd == BF_BC_REQUEST_DEATH_NOTIFICATION || cmd == BF_BC_CLEAR_DEATH_NOTIFICATION ||
        cmd == BF_BC_DEAD_BINDER_DONE) return death_command(cmd, bytes, tid);
    if (cmd == BF_BC_FREE_BUFFER) {
        uint64_t ptr; memcpy(&ptr, bytes, 8); return free_buffer(mem, ptr);
    }
    if (cmd == BF_BC_INCREFS || cmd == BF_BC_ACQUIRE ||
        cmd == BF_BC_RELEASE || cmd == BF_BC_DECREFS ||
        cmd == BF_BC_INCREFS_DONE || cmd == BF_BC_ACQUIRE_DONE)
        return reference_command(cmd, bytes);
    uint64_t buffers_size = 0;
    if (cmd == BF_BC_TRANSACTION_SG || cmd == BF_BC_REPLY_SG) {
        memcpy(&buffers_size, bytes + sizeof(bf_binder_transaction), 8);
        cmd = cmd == BF_BC_TRANSACTION_SG ? BF_BC_TRANSACTION : BF_BC_REPLY;
    }
    if (cmd != BF_BC_TRANSACTION && cmd != BF_BC_REPLY) return -EINVAL;
    bf_binder_transaction tx; memcpy(&tx, bytes, sizeof(tx));
    if (input && input->error) { fail(tid, BF_BR_FAILED_REPLY, input->error); return 0; }
    if (dbg().binder_trace)
        fprintf(stderr, "[binder] transaction tid=%d target=%llu code=%#x flags=%#x data=%llu offsets=%llu\n",
                tid, (unsigned long long)tx.target, tx.code, tx.flags,
                (unsigned long long)tx.data_size, (unsigned long long)tx.offsets_size);
    if (tx.data_size > LIMIT || tx.offsets_size > 512 || buffers_size > LIMIT || (buffers_size & 7) ||
        ((tx.data_size + 7) & ~uint64_t(7)) + tx.offsets_size + buffers_size > LIMIT ||
        (tx.flags & ~(BF_TF_ONE_WAY | BF_TF_COLLECT_NOTED_APP_OPS | BF_TF_CLEAR_BUF | 0x08u | 0x10u))) {
        fail(tid, BF_BR_FAILED_REPLY, EINVAL); return 0;
    }
    BinderNode* target = nullptr;
    std::shared_ptr<Node> caller;
    std::shared_ptr<Object> destination;
    std::shared_ptr<Node> destination_owner;
    int reply_tid = 0;
    bool accepts_fds = false;
    if (cmd == BF_BC_TRANSACTION) {
        if (tx.target == 0 && domain_->service_manager) {
            if (buffers_size) { fail(tid, BF_BR_FAILED_REPLY, EINVAL); return 0; }
            return service_transaction(tx, mem, tid, fds);
        }
        if (tx.target == 0 && domain_->manager) {
            destination_owner = domain_->promote(domain_->manager->weak_from_this());
            if (destination_owner) target = static_cast<BinderNode*>(destination_owner.get());
        }
        else if (tx.target <= UINT32_MAX) {
            auto handle = handles_.find(static_cast<uint32_t>(tx.target));
            if (handle != handles_.end() && (handle->second.strong || handle->second.temporary_strong)) {
                destination = handle->second.object;
                if (destination->alive && destination->host_service == Object::HostService::GraphicsAllocator) {
                    if (buffers_size) { fail(tid, BF_BR_FAILED_REPLY, EINVAL); return 0; }
                    return allocator_transaction(tx, mem, tid);
                }
                if (destination->alive) destination_owner = domain_->promote(destination->owner);
                if (destination_owner) target = static_cast<BinderNode*>(destination_owner.get());
            }
        }
        if (!target) { fail(tid, BF_BR_DEAD_REPLY, ENOENT); return 0; }
        accepts_fds = destination ? destination->accepts_fds : domain_->manager_accepts_fds;
    } else {
        auto& stack = calls_[tid];
        if (stack.empty()) { fail(tid, BF_BR_FAILED_REPLY, EPROTO); return 0; }
        caller = domain_->promote(stack.back().caller); reply_tid = stack.back().tid;
        if (!caller) { stack.pop_back(); fail(tid, BF_BR_DEAD_REPLY, ESRCH); return 0; }
        target = static_cast<BinderNode*>(caller.get());
        accepts_fds = stack.back().accepts_fds;
    }
    auto& queue = cmd == BF_BC_REPLY ? target->replies_[reply_tid] : target->incoming_;
    if (!target->mapping_ || target->disconnected_ || queue.size() >= MAX_EVENTS ||
        replies_[tid].size() >= MAX_EVENTS || calls_[tid].size() >= 64) {
        fail(tid, BF_BR_FAILED_REPLY, ENOMEM); return 0;
    }
    uint64_t free_bytes = 0;
    for (const auto& range : target->free_) free_bytes += range.second;
    uint64_t required = std::max<uint64_t>(8, ((tx.data_size + 7) & ~uint64_t(7)) + tx.offsets_size + buffers_size);
    if (target->queued_bytes_ > free_bytes || required > free_bytes - target->queued_bytes_) {
        fail(tid, BF_BR_FAILED_REPLY, ENOMEM); return 0;
    }
    Event event{};
    event.command = cmd == BF_BC_REPLY ? BF_BR_REPLY : BF_BR_TRANSACTION;
    event.transaction = tx;
    event.transaction.target = cmd == BF_BC_REPLY ? 0 : destination ? destination->pointer : domain_->manager_ptr;
    event.transaction.cookie = cmd == BF_BC_REPLY ? 0 : destination ? destination->cookie : domain_->manager_cookie;
    // Linux exposes a sender PID only for a synchronous incoming call. Replies
    // and one-way calls have no originating thread attached to the transaction.
    event.transaction.sender_pid = cmd == BF_BC_TRANSACTION && !(tx.flags & BF_TF_ONE_WAY)
        ? sender_pid_ : 0;
    event.transaction.sender_euid = sender_uid_;
    event.transaction.buffer = 0; event.transaction.offsets = 0;
    if (input) event.payload = std::move(input->payload);
    else {
        event.payload.resize(tx.data_size);
        if (tx.data_size) mem.read(tx.buffer, event.payload.data(), tx.data_size);
    }
    int object_error = translate_objects(*target, event, mem, tx, tid, fds, accepts_fds, buffers_size, input);
    if (object_error) { fail(tid, BF_BR_FAILED_REPLY, -object_error); return 0; }
    event.caller = weak_from_this(); event.caller_tid = tid;
    if (cmd == BF_BC_REPLY) calls_[tid].pop_back();
    target->queued_bytes_ += required;
    queue.push_back(std::move(event));
    replies_[tid].push_back(Event{BF_BR_TRANSACTION_COMPLETE});
    target->ready_.notify_all(); ready_.notify_all();
    return 0;
}
int BinderNode::write_read(uint64_t arg, Memory& mem, CPU& cpu, FdTable& fds,
                            std::unique_lock<std::mutex>& lock, const std::function<bool()>& cancelled, FdTable* source_files) {
    auto wr = mem.load<bf_binder_write_read>(arg);
    if (wr.write_size > LIMIT || wr.read_size > LIMIT ||
        wr.write_consumed > wr.write_size || wr.read_consumed > wr.read_size) return -EINVAL;
    mem.check_access(arg, sizeof(wr), Memory::GUEST_PROT_WRITE);
    if (wr.read_size) mem.check_access(wr.read_buffer, wr.read_size, Memory::GUEST_PROT_WRITE);
    std::vector<uint8_t> input(wr.write_size);
    if (!input.empty()) mem.read(wr.write_buffer, input.data(), input.size());
    int result = 0;
    while (wr.write_consumed < wr.write_size) {
        size_t pos = wr.write_consumed;
        if (wr.write_size - pos < 4) { result = -EINVAL; break; }
        uint32_t cmd; memcpy(&cmd, input.data() + pos, 4);
        size_t n = _IOC_SIZE(cmd);
        if (n > wr.write_size - pos - 4) { result = -EINVAL; break; }
        TransactionInput staged;
        bool transaction = cmd == BF_BC_TRANSACTION || cmd == BF_BC_REPLY ||
            cmd == BF_BC_TRANSACTION_SG || cmd == BF_BC_REPLY_SG;
        if (transaction) {
            // Guest copies and SG buffer staging are private to this request.
            // Other endpoints can route and receive work while these run.
            lock.unlock();
            try { staged = prepare_transaction(input.data() + pos + 4,
                    cmd == BF_BC_TRANSACTION_SG || cmd == BF_BC_REPLY_SG, mem); }
            catch (...) { lock.lock(); throw; }
            lock.lock();
            if (disconnected_ || domain_->stopped) { result = -ECONNREFUSED; break; }
        }
        try { result = command(cmd, input.data() + pos + 4, mem, cpu.tid,
                source_files ? *source_files : fds, transaction ? &staged : nullptr); }
        catch (const EmuError&) { result = -EFAULT; }
        if (!result) wr.write_consumed += 4 + n;
        // A read may block waiting for exactly the peer's death. Release
        // command-scoped promotions before waiting, outside the domain lock.
        BinderDomain::Guard::current->release_peers();
        if (result) break;
    }
    if (result) wr.read_consumed = 0;
    if (!result && wr.read_size) {
        auto available = [&] {
            return !replies_[cpu.tid].empty() ||
                (loopers_.count(cpu.tid) && calls_[cpu.tid].empty() && !incoming_.empty());
        };
        while (!available() && !domain_->stopped && !disconnected_) {
            if (flags_ & O_NONBLOCK) { result = -EAGAIN; break; }
            if (cpu.has_pending_signals() || (cancelled && cancelled())) { result = -EINTR; break; }
            if (loopers_.count(cpu.tid)) waiting_loopers_.insert(cpu.tid);
            ready_.wait_for(lock, std::chrono::milliseconds(20));
            waiting_loopers_.erase(cpu.tid);
        }
        if (domain_->stopped || disconnected_) result = -ECONNREFUSED;
        if (!result) {
            // BR_NOOP is always the first return when read_consumed starts at zero.
            if (!wr.read_consumed) {
                if (wr.read_size < 4) result = -EINVAL;
                else { mem.store<uint32_t>(wr.read_buffer, BF_BR_NOOP); wr.read_consumed = 4; }
            }
            while (!result && available()) {
                auto& queue = !replies_[cpu.tid].empty() ? replies_[cpu.tid] : incoming_;
                Event& event = queue.front();
                if (event.command == BF_BR_DEAD_BINDER && event.death->cancelled) {
                    queue.pop_front(); continue;
                }
                bool tx = event.command == BF_BR_TRANSACTION || event.command == BF_BR_REPLY;
                size_t n = 4 + (tx ? sizeof(bf_binder_transaction) : _IOC_SIZE(event.command));
                if (n > wr.read_size - wr.read_consumed) break;
                uint64_t buffer = 0;
                std::vector<int> installed_fds;
                bool synchronous = event.command == BF_BR_TRANSACTION &&
                                    !(event.transaction.flags & BF_TF_ONE_WAY);
                if (synchronous && calls_[cpu.tid].size() >= 64) { result = -EOVERFLOW; break; }
                if (tx) {
                    std::vector<uint8_t> patched_payload;
                    if (!event.descriptors.empty() || !event.fixups.empty()) patched_payload = event.payload;
                    for (const auto& descriptor : event.descriptors) {
                        int fd = fds.allocate(descriptor.node);
                        if (fd < 0) { result = fd; break; }
                        installed_fds.push_back(fd);
                        uint64_t number = static_cast<uint32_t>(fd);
                        memcpy(patched_payload.data() + descriptor.offset, &number, descriptor.width);
                        if (descriptor.width == 8) {
                            uint64_t zero = 0;
                            memcpy(patched_payload.data() + descriptor.offset + 8, &zero, 8);
                        }
                    }
                    if (!result) buffer = allocate_buffer(mem,
                            patched_payload.empty() ? event.payload : patched_payload);
                    if (!buffer) {
                        for (int fd : installed_fds) fds.close(fd);
                        if (!result) result = -ENOMEM;
                        break;
                    }
                    for (const auto& fixup : event.fixups) {
                        uint64_t pointer = buffer + fixup.target;
                        memcpy(driver_mapping_ + (buffer - mapping_) + fixup.offset, &pointer, 8);
                    }
                    event.transaction.buffer = buffer;
                    event.transaction.offsets = event.transaction.offsets_size ? buffer + ((event.transaction.data_size + 7) & ~uint64_t(7)) : 0;
                }
                uint8_t output[4 + sizeof(bf_binder_transaction)];
                memcpy(output, &event.command, 4);
                if (tx) memcpy(output + 4, &event.transaction, sizeof(event.transaction));
                else if (n > 4) memcpy(output + 4, &event.object, n - 4);
                try { mem.write(wr.read_buffer + wr.read_consumed, output, n); }
                catch (const EmuError&) {
                    if (buffer) (void)free_buffer(mem, buffer);
                    for (int fd : installed_fds) fds.close(fd);
                    result = -EFAULT; break;
                }
                if (tx && !event.references.empty()) buffer_references_[buffer] = std::move(event.references);
                for (size_t i = 0; i < installed_fds.size(); ++i)
                    if (event.descriptors[i].auto_close)
                        buffer_fds_[buffer].push_back({&fds, installed_fds[i], event.descriptors[i].node});
                bool dead = event.command == BF_BR_DEAD_BINDER;
                if (dead) event.death->delivered = true;
                if (event.command == BF_BR_CLEAR_DEATH_NOTIFICATION_DONE) {
                    auto death = event.death; death->cancelled = true;
                    deaths_.erase(std::remove(deaths_.begin(), deaths_.end(), death), deaths_.end());
                }
                if (tx) queued_bytes_ -= std::max<uint64_t>(8, (event.payload.size() + 7) & ~uint64_t(7));
                if (synchronous) calls_[cpu.tid].push_back({event.caller, event.caller_tid,
                        !!(event.transaction.flags & BF_TF_ACCEPT_FDS)});
                wr.read_consumed += n; queue.pop_front();
                // Linux stops the read after a death cookie so its recipient
                // can acknowledge it before processing subsequent work.
                if (dead || tx) break;
            }
        }
    }
    if (!result && wr.read_consumed >= 4 && loopers_.count(cpu.tid) &&
        waiting_loopers_.empty() && !requested_threads_ && registered_loopers_.size() < max_threads_) {
        ++requested_threads_;
        mem.store<uint32_t>(wr.read_buffer, BF_BR_SPAWN_LOOPER);
    }
    mem.store(arg, wr);
    return result;
}
int BinderNode::ioctl_thread(uint32_t request, uint64_t arg, Memory& mem, CPU& cpu, FdTable& fds) {
    return ioctl_broker(request, arg, mem, cpu, fds, {});
}
int BinderNode::ioctl_broker(uint32_t request, uint64_t arg, Memory& mem, CPU& cpu,
                            FdTable& fds, const std::function<bool()>& cancelled, FdTable* source_files) {
    BinderDomain::Guard guard(*domain_);
    auto& lock = guard.lock;
    if (domain_->stopped || disconnected_) return -ECONNREFUSED;
    try {
        switch (request) {
        case BF_BINDER_VERSION: mem.store<int32_t>(arg, 8); return 0;
        case BF_BINDER_SET_MAX_THREADS: max_threads_ = mem.load<uint32_t>(arg); return 0;
        case BF_BINDER_GET_EXTENDED_ERROR: {
            auto& detail = errors_[cpu.tid];
            if (!detail.command) detail.command = BF_BR_OK;
            mem.store(arg, detail); detail = {0, BF_BR_OK, 0}; return 0;
        }
        case BF_BINDER_THREAD_EXIT:
            if (!calls_[cpu.tid].empty()) return -EBUSY;
            loopers_.erase(cpu.tid); registered_loopers_.erase(cpu.tid); waiting_loopers_.erase(cpu.tid); replies_.erase(cpu.tid); calls_.erase(cpu.tid); errors_.erase(cpu.tid); return 0;
        case BF_BINDER_SET_CONTEXT_MGR:
        case BF_BINDER_SET_CONTEXT_MGR_EXT: {
            bf_flat_binder obj{};
            if (request == BF_BINDER_SET_CONTEXT_MGR_EXT) {
                obj = mem.load<bf_flat_binder>(arg);
                if (obj.type != BF_BINDER_TYPE_BINDER || (obj.flags & 0x1000)) return -EINVAL;
            } // Legacy BINDER_SET_CONTEXT_MGR ignores its argument.
            if (domain_->manager || domain_->service_manager) return -EBUSY;
            domain_->manager = this; domain_->manager_ptr = obj.binder;
            domain_->manager_accepts_fds = request == BF_BINDER_SET_CONTEXT_MGR ||
                                          (obj.flags & BF_FLAT_BINDER_FLAG_ACCEPTS_FDS);
            domain_->manager_cookie = obj.cookie; return 0;
        }
        case BF_BINDER_WRITE_READ: return write_read(arg, mem, cpu, fds, lock, cancelled, source_files);
        default: return -ENOTTY;
        }
    } catch (const EmuError&) {
        return request == BF_BINDER_VERSION || request == BF_BINDER_SET_MAX_THREADS ||
               request == BF_BINDER_SET_CONTEXT_MGR_EXT ? -EINVAL : -EFAULT;
    }
}
}
