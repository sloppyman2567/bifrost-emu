#include "core/memory.h"
#include "yggdrasil/android_parcel.hpp"
#include "yggdrasil/binder_node.hpp"
#include "debug_flags.h"
#include <cstdio>
#include <algorithm>

namespace arm64emu::yggdrasil {
namespace {
constexpr uint32_t STATUS_CODE = 0x08;
constexpr uint32_t SYSTEM_HEADER = 0x53595354; // B_PACK_CHARS('S','Y','S','T')
constexpr const char *DESCRIPTOR = "android.os.IServiceManager";
bool service_name(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '/';
    });
}
} // namespace
int BinderNode::service_transaction(const bf_binder_transaction &tx, Memory &mem, int tid) {
    // Reserve the local response before changing registry/object ownership.
    uint64_t reservation = 64;
    if (tx.code == 4) {
        reservation = 8;
        for (const auto &entry : domain_->services)
            reservation += 4 + ((2 * (entry.first.size() + 1) + 3) & ~uint64_t(3));
    }
    uint64_t free_bytes = 0;
    for (const auto &range : free_)
        free_bytes += range.second;
    if (dbg().binder_trace)
        fprintf(stderr, "[binder] registry tid=%d code=%#x flags=%#x data=%llu offsets=%llu mapped=%d memory=%d free=%llu queued=%llu\n",
                tid, tx.code, tx.flags, (unsigned long long)tx.data_size,
                (unsigned long long)tx.offsets_size, !!mapping_, memory_ == &mem,
                (unsigned long long)free_bytes, (unsigned long long)queued_bytes_);
    if (!mapping_ || memory_ != &mem || tx.flags & BF_TF_ONE_WAY ||
        replies_[tid].size() + 2 > 256 || queued_bytes_ > free_bytes ||
        reservation > free_bytes - queued_bytes_) {
        fail(tid, BF_BR_FAILED_REPLY, ENOMEM);
        return 0;
    }
    Event reply(BF_BR_REPLY);
    reply.transaction.code = tx.code;
    AndroidParcelWriter result;
    try {
        std::vector<uint8_t> data(tx.data_size);
        if (!data.empty())
            mem.read(tx.buffer, data.data(), data.size());
        AndroidParcelReader input(data);
        if (tx.code == 0x5f504e47) { // PING_TRANSACTION: no interface token
            input.finish();
            if (tx.offsets_size)
                throw std::invalid_argument("unexpected Binder objects");
            result.write<int32_t>(0);
            reply.transaction.flags = STATUS_CODE;
        } else if (tx.code == 0x5f4e5446) { // INTERFACE_TRANSACTION
            input.finish();
            if (tx.offsets_size)
                throw std::invalid_argument("unexpected Binder objects");
            result.ascii_string(DESCRIPTOR);
        } else {
            input.read<int32_t>(); // StrictMode policy
            input.read<int32_t>(); // WorkSource UID
            if (input.read<uint32_t>() != SYSTEM_HEADER || input.ascii_string(64) != DESCRIPTOR)
                throw std::invalid_argument("wrong service-manager interface");
            if (tx.code < 1 || tx.code > 4) {
                result.write<int32_t>(-EBADMSG); // Android UNKNOWN_TRANSACTION
                reply.transaction.flags = STATUS_CODE;
            } else if (tx.code == 4) { // listServices(dumpPriority)
                uint32_t priority = input.read<uint32_t>();
                input.finish();
                if (tx.offsets_size)
                    throw std::invalid_argument("unexpected Binder objects");
                result.write<int32_t>(0); // AIDL Status: no exception
                std::vector<std::string> names;
                for (const auto &entry : domain_->services)
                    if (entry.second.object->alive && !entry.second.object->owner.expired() &&
                        (entry.second.dump_priority & priority))
                        names.push_back(entry.first);
                result.write<int32_t>(names.size());
                for (const auto &name : names)
                    result.ascii_string(name);
            } else {
                std::string name = input.ascii_string(127);
                if (!service_name(name))
                    throw std::invalid_argument("invalid service name");
                if (tx.code == 3) { // addService(name, binder, allowIsolated, dumpPriority)
                    uint64_t offset = input.position();
                    auto flat = input.read<bf_flat_binder>();
                    int32_t isolated = input.read<int32_t>();
                    uint32_t priority = input.read<uint32_t>();
                    input.finish();
                    if (tx.offsets_size != 8 || mem.load<uint64_t>(tx.offsets) != offset ||
                        (flat.type != BF_BINDER_TYPE_BINDER &&
                         flat.type != BF_BINDER_TYPE_HANDLE) ||
                        !flat.binder || (isolated != 0 && isolated != 1) || (priority & ~31u))
                        throw std::invalid_argument("invalid service registration");
                    auto existing = domain_->services.find(name);
                    if (existing == domain_->services.end() && domain_->services.size() >= 256)
                        throw std::invalid_argument("service registry full");
                    std::shared_ptr<Node> replaced_owner;
                    if (existing != domain_->services.end()) {
                        // Keep promotion alive through replacement: teardown
                        // removes owned services and could invalidate existing.
                        replaced_owner = existing->second.object->owner.lock();
                        if (replaced_owner &&
                            static_cast<BinderNode *>(replaced_owner.get())->incoming_.size() + 2 > 256)
                            throw std::invalid_argument("service owner queue full");
                    }
                    Event incoming;
                    incoming.payload = std::move(data);
                    int error = translate_objects(*this, incoming, mem, tx);
                    if (error) {
                        result.write<int32_t>(error);
                        reply.transaction.flags = STATUS_CODE;
                    } else {
                        auto object = incoming.references.at(0).object;
                        change_reference(object, 1, 0); // Registry owns a strong reference.
                        if (existing != domain_->services.end())
                            change_reference(existing->second.object, -1, 0);
                        domain_->services[name] = {object, priority};
                        for (const auto &ref : incoming.references) {
                            --handles_.at(ref.handle).temporary_strong;
                            change_reference(ref.object, -1, 0);
                        }
                        result.write<int32_t>(0);
                    }
                } else { // getService/checkService: both nonblocking in API 35.
                    input.finish();
                    if (tx.offsets_size)
                        throw std::invalid_argument("unexpected Binder objects");
                    result.write<int32_t>(0);
                    auto found = domain_->services.find(name);
                    bf_flat_binder flat{BF_BINDER_TYPE_BINDER, 0, 0, 0};
                    if (found != domain_->services.end() && found->second.object->alive &&
                        !found->second.object->owner.expired()) {
                        auto object = found->second.object;
                        auto handle =
                            std::find_if(handles_.begin(), handles_.end(), [&](const auto &entry) {
                                return entry.second.object == object;
                            });
                        if (object->strong >= 1000000 ||
                            (handle == handles_.end() &&
                             (handles_.size() >= 1024 || !next_handle_)))
                            throw std::invalid_argument("service handle limit");
                        uint32_t id;
                        if (handle == handles_.end()) {
                            id = next_handle_++;
                            Handle value;
                            value.object = object;
                            handles_[id] = value;
                        } else
                            id = handle->first;
                        ++handles_.at(id).temporary_strong;
                        change_reference(object, 1, 0);
                        reply.references.push_back({id, false, object});
                        if (object->owner.lock().get() == this) {
                            flat.binder = object->pointer;
                            flat.cookie = object->cookie;
                        } else {
                            flat.type = BF_BINDER_TYPE_HANDLE;
                            flat.binder = id;
                        }
                    }
                    uint64_t offset = result.bytes.size();
                    result.write(flat);
                    reply.transaction.data_size = result.bytes.size();
                    if (flat.binder) {
                        reply.transaction.offsets_size = 8;
                        while (result.bytes.size() & 7)
                            result.write<uint8_t>(0);
                        result.write(offset);
                    }
                }
            }
        }
    } catch (const std::invalid_argument &) {
        result.bytes.clear();
        result.write<int32_t>(-EINVAL);
        reply.transaction.flags = STATUS_CODE;
    }
    if (!reply.transaction.data_size)
        reply.transaction.data_size = result.bytes.size();
    reply.payload = std::move(result.bytes);
    queued_bytes_ += std::max<uint64_t>(8, (reply.payload.size() + 7) & ~uint64_t(7));
    replies_[tid].push_back(Event{BF_BR_TRANSACTION_COMPLETE});
    replies_[tid].push_back(std::move(reply));
    domain_->ready.notify_all();
    return 0;
}
} // namespace arm64emu::yggdrasil
