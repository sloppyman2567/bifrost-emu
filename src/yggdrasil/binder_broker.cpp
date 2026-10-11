#include "yggdrasil/binder_broker.hpp"
#include "yggdrasil/binder_node.hpp"
#include "yggdrasil/host_node.hpp"
#include "yggdrasil/yggdrasil.hpp"
#include "core/memory.h"
#include "core/cpu.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <map>
#include <poll.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <system_error>

namespace arm64emu::yggdrasil {
namespace {
constexpr uint32_t MAGIC = 0x41534744, VERSION = 2;
constexpr size_t LIMIT = 16 * 1024 * 1024, MAX_FDS = 64, MAX_CHUNKS = 256;
enum Operation : uint32_t { OPEN = 1, MAP, IOCTL, UNMAP, PING, STOP };
using Key = std::array<uint64_t, 2>;
struct Header { uint32_t magic = MAGIC, version = VERSION, operation = 0, size = 0; Key key{}; };
struct Open { uint32_t device, flags, uid; int32_t pid; };
struct Mapping { uint64_t address, size; };
struct Request { uint64_t argument; uint32_t request, tid, flags, chunks, fds, invalid_fds; };
struct Chunk { uint64_t address; uint32_t size, input; };
struct FdPatch { uint64_t address; uint32_t width, auto_close; uint64_t buffer; };
struct Result { int64_t value; uint32_t chunks = 0, fds = 0; };
struct Packet {
    Header header;
    std::vector<uint8_t> body;
    std::vector<int> fds;
    ~Packet() { for (int fd : fds) if (fd >= 0) ::close(fd); }
};
template<class T> void append(std::vector<uint8_t>& bytes, const T& value) {
    auto p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
void append_bytes(std::vector<uint8_t>& bytes, const void* source, size_t size) {
    auto p = static_cast<const uint8_t*>(source);
    bytes.insert(bytes.end(), p, p + size);
}
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t position = 0;
    template<class T> T take() {
        if (sizeof(T) > bytes.size() - position) throw EmuError("short Asgard packet");
        T result; memcpy(&result, bytes.data() + position, sizeof(T)); position += sizeof(T); return result;
    }
    const uint8_t* data(size_t size) {
        if (size > bytes.size() - position) throw EmuError("short Asgard data");
        auto result = bytes.data() + position; position += size; return result;
    }
    void finish() { if (position != bytes.size()) throw EmuError("trailing Asgard data"); }
};
bool transfer(int fd, void* buffer, size_t size, bool writing) {
    auto p = static_cast<uint8_t*>(buffer);
    while (size) {
        ssize_t n = writing ? ::send(fd, p, size, MSG_NOSIGNAL) : ::recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
bool send_packet(int fd, Header header, const std::vector<uint8_t>& body,
                 const std::vector<int>& fds = {}) {
    if (body.size() > LIMIT || fds.size() > MAX_FDS) return false;
    header.size = body.size();
    iovec vector{&header, sizeof(header)};
    alignas(cmsghdr) char control[CMSG_SPACE(MAX_FDS * sizeof(int))]{};
    msghdr message{}; message.msg_iov = &vector; message.msg_iovlen = 1;
    if (!fds.empty()) {
        message.msg_control = control; message.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        auto cmsg = CMSG_FIRSTHDR(&message); cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int)); memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));
    }
    ssize_t n;
    do { n = ::sendmsg(fd, &message, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n <= 0 || static_cast<size_t>(n) > sizeof(header)) return false;
    if (!transfer(fd, reinterpret_cast<uint8_t*>(&header) + n, sizeof(header) - n, true)) return false;
    return body.empty() || transfer(fd, const_cast<uint8_t*>(body.data()), body.size(), true);
}
bool receive_packet(int fd, Packet& packet) {
    iovec vector{&packet.header, sizeof(Header)};
    alignas(cmsghdr) char control[CMSG_SPACE(MAX_FDS * sizeof(int))]{};
    msghdr message{}; message.msg_iov = &vector; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    ssize_t n;
    do { n = ::recvmsg(fd, &message, MSG_WAITALL | MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    for (auto cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        auto files = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        packet.fds.insert(packet.fds.end(), files, files + count);
    }
    if (n != sizeof(Header) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        packet.header.magic != MAGIC || packet.header.version != VERSION || packet.header.size > LIMIT) return false;
    packet.body.resize(packet.header.size);
    return packet.body.empty() || transfer(fd, packet.body.data(), packet.body.size(), false);
}
int connect_socket(const std::string& path) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        int error = errno; ::close(fd); errno = error; return -1;
    }
    return fd;
}
bool hung_up(int fd) {
    pollfd p{fd, POLLRDHUP, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLRDHUP | POLLHUP | POLLERR));
}
void chunk(std::vector<uint8_t>& body, Memory& memory, uint64_t address, size_t size, bool input) {
    if (size > LIMIT || address > UINT64_MAX - size) throw EmuError("oversized Asgard memory range");
    memory.check_access(address, size, input ? Memory::GUEST_PROT_READ : Memory::GUEST_PROT_WRITE);
    append(body, Chunk{address, static_cast<uint32_t>(size), input ? 1u : 0u});
    if (input && size) {
        size_t begin = body.size(); body.resize(begin + size); memory.read(address, body.data() + begin, size);
    }
    if (body.size() > LIMIT) throw EmuError("oversized Asgard request");
}
class RemoteBinder final : public Node {
    std::string path_;
    Key key_{};
    int control_, owner_;
    std::atomic<int> flags_;
    uint64_t mapping_ = 0, size_ = 0;
    uint8_t* alias_ = nullptr;
    std::mutex mapping_mutex_, pool_mutex_;
    std::vector<int> idle_connections_;
    struct AutoFd { FdTable* table; int number; std::weak_ptr<Node> file; };
    std::map<uint64_t, std::vector<AutoFd>> buffer_fds_;
    void free_local_buffer(uint64_t buffer) {
        auto found = buffer_fds_.find(buffer);
        if (found == buffer_fds_.end()) return;
        for (auto& file : found->second)
            if (file.table->get(file.number) == file.file.lock()) file.table->close(file.number);
        buffer_fds_.erase(found);
    }
    int call(Operation operation, const std::vector<uint8_t>& body, const std::vector<int>& files,
             Packet& reply, CPU* cpu = nullptr) {
        if (::getpid() != owner_) return -EBADF; // Binder opens belong to the opening process.
        int fd = -1;
        { std::lock_guard<std::mutex> lock(pool_mutex_);
          if (!idle_connections_.empty()) { fd = idle_connections_.back(); idle_connections_.pop_back(); } }
        if (fd < 0) fd = connect_socket(path_);
        if (fd < 0) return -errno;
        Header header; header.operation = operation; header.key = key_;
        int error = 0;
        if (!send_packet(fd, header, body, files)) error = -EPIPE;
        while (!error) {
            pollfd p{fd, POLLIN, 0}; int ready = ::poll(&p, 1, 20);
            if (ready < 0 && errno != EINTR) { error = -errno; break; }
            if (ready > 0) {
                if (!receive_packet(fd, reply)) error = -EPIPE;
                break;
            }
            if (cpu && (cpu->has_pending_signals() || !cpu->running)) { error = -EINTR; break; }
        }
        if (!error && (reply.header.operation != operation || reply.header.key != key_)) error = -EPROTO;
        if (!error) {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            if (idle_connections_.size() < 8) { idle_connections_.push_back(fd); fd = -1; }
        }
        if (fd >= 0) ::close(fd);
        return error;
    }
public:
    RemoteBinder(std::string path, Key key, int control, int flags)
        : path_(std::move(path)), key_(key), control_(control), owner_(::getpid()), flags_(flags) {}
    ~RemoteBinder() override {
        while (!buffer_fds_.empty()) free_local_buffer(buffer_fds_.begin()->first);
        if (alias_) ::munmap(alias_, size_);
        for (int fd : idle_connections_) ::close(fd);
        ::close(control_);
    }
    ssize_t read(uint64_t, void*, size_t) override { return -EINVAL; }
    ssize_t write(uint64_t, const void*, size_t) override { return -EINVAL; }
    ssize_t lseek(int64_t, int) override { return -ESPIPE; }
    bool seekable() const override { return false; }
    int flags() const override { return flags_; }
    void set_flags(int flags) override { flags_ = (flags_ & ~O_NONBLOCK) | (flags & O_NONBLOCK); }
    int fstat(struct stat* st) override { memset(st, 0, sizeof(*st)); st->st_mode = S_IFCHR | 0666; st->st_nlink = 1; return 0; }
    bool owns_guest_mappings() const override { return true; }
    int protect_mapping(uint64_t address, uint64_t size, uint64_t prot) override {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        return mapping_ && size && address < mapping_ + size_ && mapping_ < address + size && (prot & 6) ? -EACCES : 0;
    }
    void invalidate_mapping(uint64_t address, uint64_t size) override {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        if (!mapping_ || !size || address >= mapping_ + size_ || mapping_ >= address + size) return;
        std::vector<uint8_t> body; append(body, Mapping{mapping_, size_}); Packet reply;
        (void)call(UNMAP, body, {}, reply);
        while (!buffer_fds_.empty()) free_local_buffer(buffer_fds_.begin()->first);
        ::munmap(alias_, size_); alias_ = nullptr; mapping_ = size_ = 0;
    }
    int64_t mmap_guest(Memory& memory, uint64_t hint, uint64_t length, uint64_t prot,
                       uint64_t flags, uint64_t offset) override {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        if (mapping_) return -EBUSY;
        if (!length || length > 4 * 1024 * 1024 || offset || prot != 1 || (flags & 3) != 2 ||
            (flags & (0x10 | 0x100000))) return -EINVAL;
        uint64_t size = (length + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
        uint64_t address = memory.mmap_alloc(size, hint, false, 1, true);
        if (!address) return -ENOMEM;
        std::vector<uint8_t> body; append(body, Mapping{address, size}); Packet reply;
        int error = call(MAP, body, {}, reply);
        int64_t result = error;
        if (!error) { Reader input{reply.body}; result = input.take<Result>().value; }
        if (result < 0 || reply.fds.size() != 1) { memory.untrack_allocation(address, size); return result < 0 ? result : -EPROTO; }
        void* alias = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, reply.fds[0], 0);
        if (alias == MAP_FAILED || !memory.map_device_readonly(address, size, reply.fds[0])) {
            if (alias != MAP_FAILED) ::munmap(alias, size);
            Packet rollback; (void)call(UNMAP, body, {}, rollback);
            memory.untrack_allocation(address, size); return -ENOMEM;
        }
        mapping_ = address; size_ = size; alias_ = static_cast<uint8_t*>(alias); return address;
    }
    int ioctl_thread(uint32_t request, uint64_t argument, Memory& memory, CPU& cpu, FdTable& fds) override {
        try {
            Request rpc{argument, request, static_cast<uint32_t>(cpu.tid), static_cast<uint32_t>(flags_), 0, 0, 0};
            std::vector<uint8_t> body(sizeof(rpc)); std::vector<int> files; std::vector<int32_t> numbers, invalid_numbers;
            std::vector<std::shared_ptr<Node>> source_files;
            auto add = [&](uint64_t address, size_t size, bool input) { chunk(body, memory, address, size, input); ++rpc.chunks; };
            auto add_fd = [&](int32_t number) {
                if (std::find(numbers.begin(), numbers.end(), number) != numbers.end()) return;
                auto node = fds.get(number);
                if (!node || node->host_fd() < 0) { invalid_numbers.push_back(number); return; }
                numbers.push_back(number); files.push_back(node->host_fd()); source_files.push_back(std::move(node));
            };
            std::vector<std::pair<size_t, uint64_t>> freed_buffers;
            if (request == BF_BINDER_WRITE_READ) {
                auto wr = memory.load<bf_binder_write_read>(argument);
                if (wr.write_size > 1024 * 1024 || wr.read_size > 1024 * 1024 ||
                    wr.write_consumed > wr.write_size || wr.read_consumed > wr.read_size) return -EINVAL;
                memory.check_access(argument, sizeof(wr), Memory::GUEST_PROT_WRITE);
                add(argument, sizeof(wr), true);
                if (wr.read_size) add(wr.read_buffer, wr.read_size, false);
                if (wr.write_size) add(wr.write_buffer, wr.write_size, true);
                std::vector<uint8_t> commands(wr.write_size);
                if (!commands.empty()) memory.read(wr.write_buffer, commands.data(), commands.size());
                for (size_t at = wr.write_consumed; at < commands.size();) {
                    if (commands.size() - at < 4) return -EINVAL;
                    uint32_t command; memcpy(&command, commands.data() + at, 4);
                    size_t bytes = _IOC_SIZE(command);
                    if (bytes > commands.size() - at - 4) return -EINVAL;
                    if (command == BF_BC_FREE_BUFFER) {
                        uint64_t buffer; memcpy(&buffer, commands.data() + at + 4, 8);
                        freed_buffers.push_back({at + 4 + bytes, buffer});
                    }
                    if (command == BF_BC_TRANSACTION || command == BF_BC_REPLY ||
                        command == BF_BC_TRANSACTION_SG || command == BF_BC_REPLY_SG) {
                        bf_binder_transaction tx; memcpy(&tx, commands.data() + at + 4, sizeof(tx));
                        if (tx.data_size > 1024 * 1024 || tx.offsets_size > 512 || (tx.offsets_size & 7)) return -EINVAL;
                        if (tx.data_size) add(tx.buffer, tx.data_size, true);
                        if (tx.offsets_size) add(tx.offsets, tx.offsets_size, true);
                        for (size_t i = 0; i < tx.offsets_size; i += 8) {
                            auto off = memory.load<uint64_t>(tx.offsets + i);
                            if (off > tx.data_size || sizeof(bf_flat_binder) > tx.data_size - off) return -EINVAL;
                            auto type = memory.load<uint32_t>(tx.buffer + off);
                            if (type == BF_BINDER_TYPE_FD) add_fd(memory.load<int32_t>(tx.buffer + off + 8));
                            else if (type == BF_BINDER_TYPE_PTR) {
                                if (sizeof(bf_binder_buffer) > tx.data_size - off) return -EINVAL;
                                auto buffer = memory.load<bf_binder_buffer>(tx.buffer + off);
                                if (buffer.length > 1024 * 1024) return -EINVAL;
                                if (buffer.length) add(buffer.buffer, buffer.length, true);
                            } else if (type == BF_BINDER_TYPE_FDA) {
                                if (sizeof(bf_binder_fd_array) > tx.data_size - off) return -EINVAL;
                                auto array = memory.load<bf_binder_fd_array>(tx.buffer + off);
                                if (array.parent >= i / 8 || array.num_fds > MAX_FDS) return -EINVAL;
                                auto parent_offset = memory.load<uint64_t>(tx.offsets + array.parent * 8);
                                if (parent_offset > tx.data_size || sizeof(bf_binder_buffer) > tx.data_size - parent_offset) return -EINVAL;
                                auto parent = memory.load<bf_binder_buffer>(tx.buffer + parent_offset);
                                if (parent.type != BF_BINDER_TYPE_PTR || array.parent_offset > parent.length ||
                                    array.num_fds > (parent.length - array.parent_offset) / 4) return -EINVAL;
                                for (uint64_t j = 0; j < array.num_fds; ++j)
                                    add_fd(memory.load<int32_t>(parent.buffer + array.parent_offset + j * 4));
                            }
                        }
                    }
                    at += 4 + bytes;
                }
            } else if (request == BF_BINDER_VERSION) add(argument, 4, false);
            else if (request == BF_BINDER_SET_MAX_THREADS) add(argument, 4, true);
            else if (request == BF_BINDER_SET_CONTEXT_MGR_EXT) add(argument, sizeof(bf_flat_binder), true);
            else if (request == BF_BINDER_GET_EXTENDED_ERROR) add(argument, sizeof(bf_binder_error), false);
            else if (request != BF_BINDER_THREAD_EXIT && request != BF_BINDER_SET_CONTEXT_MGR) return -ENOTTY;
            if (rpc.chunks > MAX_CHUNKS || files.size() > MAX_FDS || invalid_numbers.size() > MAX_FDS) return -E2BIG;
            rpc.fds = files.size(); rpc.invalid_fds = invalid_numbers.size(); memcpy(body.data(), &rpc, sizeof(rpc));
            for (int32_t number : numbers) append(body, number);
            for (int32_t number : invalid_numbers) append(body, number);
            Packet reply; int error = call(IOCTL, body, files, reply, &cpu); if (error) return error;
            Reader input{reply.body}; auto result = input.take<Result>();
            if (result.chunks > MAX_CHUNKS || result.fds != reply.fds.size()) return -EPROTO;
            for (unsigned i = 0; i < result.chunks; i++) {
                auto part = input.take<Chunk>(); auto data = input.data(part.size); memory.write(part.address, data, part.size);
            }
            {
                std::lock_guard<std::mutex> lock(mapping_mutex_);
                if (request == BF_BINDER_WRITE_READ) {
                    auto consumed = memory.load<bf_binder_write_read>(argument).write_consumed;
                    for (auto& freed : freed_buffers) if (freed.first <= consumed) free_local_buffer(freed.second);
                }
                std::vector<std::pair<FdPatch, int>> installed;
                for (unsigned i = 0; i < result.fds; i++) {
                    auto patch = input.take<FdPatch>();
                    if (!mapping_ || (patch.width != 4 && patch.width != 8) ||
                        patch.address < mapping_ || patch.width > size_ || patch.address - mapping_ > size_ - patch.width)
                        throw EmuError("invalid Asgard fd patch");
                    auto file = std::make_shared<HostNode>(reply.fds[i], O_RDWR); reply.fds[i] = -1;
                    int number = fds.allocate(file);
                    if (number < 0) {
                        for (auto& fd : installed) fds.close(fd.second);
                        return number;
                    }
                    installed.push_back({patch, number});
                }
                input.finish();
                for (auto& fd : installed) {
                    uint64_t number = static_cast<uint32_t>(fd.second);
                    memcpy(alias_ + fd.first.address - mapping_, &number, fd.first.width);
                    if (fd.first.auto_close)
                        buffer_fds_[fd.first.buffer].push_back({&fds, fd.second, fds.get(fd.second)});
                }
            }
            input.finish(); return result.value;
        } catch (const EmuError&) { return -EFAULT; }
    }
};
struct Endpoint {
    Memory memory;
    FdTable files;
    std::shared_ptr<BinderNode> node;
    std::mutex mutex;
    std::map<int, std::shared_ptr<CPU>> cpus;
};
struct Broker {
    std::mutex mutex;
    std::map<Key, std::shared_ptr<Endpoint>> endpoints;
    std::array<std::shared_ptr<BinderDomain>, 3> domains;
    std::string path;
    std::atomic<unsigned> connections{0};
    Broker(std::string path) : path(std::move(path)) {
        for (auto& domain : domains) domain = std::make_shared<BinderDomain>();
        domains[0]->service_manager = true;
    }
    void serve(int fd) {
        struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
        ucred credentials{}; socklen_t size = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || credentials.uid != ::getuid()) return;
        timeval receive_timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        bool first = true;
        for (;;) {
        Packet packet; if (!receive_packet(fd, packet)) return;
        if (first) {
            receive_timeout = {0, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
            first = false;
        }
        try {
            Reader input{packet.body}; std::vector<uint8_t> output; std::vector<int> rights;
            if (packet.header.operation == PING || packet.header.operation == STOP) {
                input.finish(); append(output, Result{0}); send_packet(fd, packet.header, output);
                if (packet.header.operation == STOP) { ::unlink(path.c_str()); ::_exit(0); }
                return;
            }
            if (packet.header.operation == OPEN) {
                auto options = input.take<Open>(); input.finish();
                if (options.device >= domains.size() || options.pid <= 0 || options.flags & ~(O_RDWR | O_NONBLOCK | O_CLOEXEC)) return;
                auto endpoint = std::make_shared<Endpoint>();
                endpoint->node = std::make_shared<BinderNode>(domains[options.device], options.flags);
                endpoint->node->set_sender(options.uid, options.pid);
                Key key; if (::getrandom(key.data(), sizeof(key), 0) != sizeof(key)) return;
                { std::lock_guard<std::mutex> lock(mutex); if (endpoints.size() >= 128) return; endpoints[key] = endpoint; }
                packet.header.key = key; append(output, Result{0});
                if (send_packet(fd, packet.header, output)) {
                    char ignored; while (::recv(fd, &ignored, 1, 0) < 0 && errno == EINTR) {}
                }
                endpoint->node->disconnect();
                { std::lock_guard<std::mutex> lock(mutex); endpoints.erase(key); }
                return;
            }
            std::shared_ptr<Endpoint> endpoint;
            { std::lock_guard<std::mutex> lock(mutex); auto it = endpoints.find(packet.header.key); if (it != endpoints.end()) endpoint = it->second; }
            if (!endpoint) { append(output, Result{-ESTALE}); send_packet(fd, packet.header, output); return; }
            if (packet.header.operation == MAP) {
                auto mapping = input.take<Mapping>(); input.finish();
                auto result = endpoint->node->map_broker_arena(endpoint->memory, mapping.address, mapping.size);
                append(output, Result{result}); if (result >= 0) rights.push_back(endpoint->node->arena_fd());
            } else if (packet.header.operation == UNMAP) {
                auto mapping = input.take<Mapping>(); input.finish(); endpoint->node->invalidate_mapping(mapping.address, mapping.size); append(output, Result{0});
            } else if (packet.header.operation == IOCTL) {
                auto request = input.take<Request>();
                if (request.chunks > MAX_CHUNKS || request.fds != packet.fds.size() || request.invalid_fds > MAX_FDS || !request.tid) return;
                std::vector<Chunk> chunks;
                size_t total = 0;
                for (unsigned i = 0; i < request.chunks; i++) {
                    auto part = input.take<Chunk>();
                    if (part.size > LIMIT - total || part.address > UINT64_MAX - part.size) return;
                    total += part.size;
                    if (!endpoint->memory.map_range(part.address, part.size)) return;
                    if (part.input) endpoint->memory.write(part.address, input.data(part.size), part.size);
                    chunks.push_back(part);
                }
                // Source numbers are scoped to this RPC. Concurrent sends can
                // capture different open files at a reused guest fd number.
                FdTable source_files;
                source_files.close_range(0, INT32_MAX);
                for (unsigned i = 0; i < request.fds; i++) {
                    int32_t number = input.take<int32_t>(); if (number < 0 || number > 1048576) return;
                    auto node = std::make_shared<HostNode>(packet.fds[i], O_RDWR); packet.fds[i] = -1;
                    int allocated = source_files.allocate(node);
                    if (allocated < 0) return;
                    if (allocated != number) { source_files.dup2(allocated, number); source_files.close(allocated); }
                }
                for (unsigned i = 0; i < request.invalid_fds; ++i) (void)input.take<int32_t>();
                input.finish();
                std::shared_ptr<CPU> cpu;
                {
                    std::lock_guard<std::mutex> lock(endpoint->mutex);
                    auto found = endpoint->cpus.find(request.tid);
                    if (found == endpoint->cpus.end()) {
                        if (endpoint->cpus.size() >= 1024) throw EmuError("too many Asgard Binder threads");
                        found = endpoint->cpus.emplace(request.tid, std::make_shared<CPU>()).first;
                    }
                    cpu = found->second;
                }
                cpu->tid = request.tid; endpoint->node->set_flags(request.flags);
                auto result = endpoint->node->ioctl_broker(request.request, request.argument, endpoint->memory, *cpu, endpoint->files, [&] { return hung_up(fd); }, &source_files);
                Result reply{result}; std::vector<uint8_t> data;
                auto returned = [&](uint64_t address, uint32_t length) {
                    append(data, Chunk{address, length, 1}); size_t begin = data.size(); data.resize(begin + length);
                    endpoint->memory.read(address, data.data() + begin, length); ++reply.chunks;
                };
                std::vector<int> installed;
                std::vector<FdPatch> patches;
                if (request.request == BF_BINDER_WRITE_READ) {
                    auto wr = endpoint->memory.load<bf_binder_write_read>(request.argument);
                    returned(request.argument, sizeof(wr));
                    if (wr.read_consumed <= wr.read_size && wr.read_consumed) {
                        returned(wr.read_buffer, wr.read_consumed);
                        for (uint64_t at = 0; at + 4 <= wr.read_consumed;) {
                            auto command = endpoint->memory.load<uint32_t>(wr.read_buffer + at); size_t size = _IOC_SIZE(command);
                            if (size > wr.read_consumed - at - 4) break;
                            if (command == BF_BR_TRANSACTION || command == BF_BR_REPLY) {
                                auto tx = endpoint->memory.load<bf_binder_transaction>(wr.read_buffer + at + 4);
                                for (uint64_t i = 0; i < tx.offsets_size; i += 8) {
                                    auto off = endpoint->memory.load<uint64_t>(tx.offsets + i);
                                    auto type = endpoint->memory.load<uint32_t>(tx.buffer + off);
                                    auto export_fd = [&](uint64_t address, uint32_t width, bool auto_close) {
                                        int number = endpoint->memory.load<int32_t>(address);
                                        auto file = endpoint->files.get(number);
                                        if (!file || file->host_fd() < 0 || rights.size() >= MAX_FDS) throw EmuError("invalid Asgard descriptor");
                                        rights.push_back(file->host_fd()); installed.push_back(number);
                                        patches.push_back({address, width, auto_close ? 1u : 0u, tx.buffer});
                                    };
                                    if (type == BF_BINDER_TYPE_FD) export_fd(tx.buffer + off + 8, 8, false);
                                    else if (type == BF_BINDER_TYPE_FDA) {
                                        auto array = endpoint->memory.load<bf_binder_fd_array>(tx.buffer + off);
                                        auto parent_offset = endpoint->memory.load<uint64_t>(tx.offsets + array.parent * 8);
                                        auto parent = endpoint->memory.load<bf_binder_buffer>(tx.buffer + parent_offset);
                                        for (uint64_t j = 0; j < array.num_fds; ++j)
                                            export_fd(parent.buffer + array.parent_offset + j * 4, 4, true);
                                    }
                                }
                            }
                            at += 4 + size;
                        }
                    }
                } else for (const auto& part : chunks) if (!part.input) returned(part.address, part.size);
                reply.fds = rights.size(); append(output, reply); append_bytes(output, data.data(), data.size());
                for (auto& patch : patches) append(output, patch);
                bool sent = send_packet(fd, packet.header, output, rights);
                for (int number : installed) endpoint->files.close(number);
                if (!sent) return;
                continue;
            } else return;
            if (!send_packet(fd, packet.header, output, rights)) return;
        } catch (const EmuError&) {
            std::vector<uint8_t> output; append(output, Result{-EFAULT});
            if (!send_packet(fd, packet.header, output)) return;
        }
        }
    }
};
} // namespace
std::unique_ptr<Node> open_broker_binder(const std::string& path, const std::string& device,
                                        int flags, uint32_t uid, int pid, int* error) {
    int fd = connect_socket(path); if (fd < 0) { *error = -errno; return nullptr; }
    Header header; header.operation = OPEN;
    Open options{device == "/dev/binder" ? 0u : device == "/dev/hwbinder" ? 1u : 2u,
                 static_cast<uint32_t>(flags & (O_RDWR | O_NONBLOCK | O_CLOEXEC)), uid, pid};
    std::vector<uint8_t> body; append(body, options); Packet reply;
    if (!send_packet(fd, header, body) || !receive_packet(fd, reply) || reply.body.size() != sizeof(Result)) {
        ::close(fd); *error = -ECONNREFUSED; return nullptr;
    }
    return std::make_unique<RemoteBinder>(path, reply.header.key, fd, flags);
}
int asgard_broker_main(const std::string& path) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) { fprintf(stderr, "Asgard: socket path is too long\n"); return 2; }
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t saved = ::umask(0077);
    int bound = fd < 0 ? -1 : ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::umask(saved);
    if (bound || ::listen(fd, 64)) { fprintf(stderr, "Asgard: %s: %s\n", path.c_str(), strerror(errno)); if (fd >= 0) ::close(fd); return 1; }
    auto broker = std::make_shared<Broker>(path);
    printf("ASGARD_BROKER_READY %s\n", path.c_str()); fflush(stdout);
    while (true) {
        int peer = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) { if (errno == EINTR) continue; break; }
        if (broker->connections.fetch_add(1) >= 512) {
            --broker->connections; ::close(peer); continue;
        }
        try {
            std::thread([broker, peer] {
                // Allocation or worker failure must disconnect one client,
                // rather than terminate every guest in the shared realm.
                try { broker->serve(peer); } catch (const std::exception&) {}
                --broker->connections;
            }).detach();
        } catch (const std::system_error&) {
            --broker->connections; ::close(peer);
        }
    }
    ::close(fd); ::unlink(path.c_str()); return 1;
}
} // namespace arm64emu::yggdrasil
