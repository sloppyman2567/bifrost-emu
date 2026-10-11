#include "yggdrasil/binder_node.hpp"
#include "core/memory.h"
#include "yggdrasil/yggdrasil.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace arm64emu::yggdrasil {
namespace { constexpr size_t MAX_OBJECTS = 1024, MAX_PARCEL_OBJECTS = 64; }
void BinderNode::prune_death(Handle& handle) {
    if (!handle.death || handle.strong || handle.weak || handle.temporary_strong || handle.temporary_weak) return;
    auto death = std::move(handle.death); death->cancelled = true;
    deaths_.erase(std::remove(deaths_.begin(), deaths_.end(), death), deaths_.end());
}
void BinderNode::notify_death(const std::shared_ptr<BinderDeath>& death) {
    if (death->queued || death->cancelled) return;
    auto node = domain_->promote(death->recipient);
    if (!node) return;
    auto recipient = static_cast<BinderNode*>(node.get());
    if (recipient->disconnected_) return;
    death->queued = true;
    Event event(BF_BR_DEAD_BINDER); event.death = death; event.object.ptr = death->cookie;
    recipient->incoming_.push_back(std::move(event));
    recipient->ready_.notify_all();
}
int BinderNode::death_command(uint32_t command, const uint8_t* bytes, int tid) {
    auto clear_done = [&](const std::shared_ptr<BinderDeath>& death) {
        Event event(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE);
        event.death = death; event.object.ptr = death->cookie;
        auto& queue = loopers_.count(tid) ? replies_[tid] : incoming_;
        queue.push_back(std::move(event)); ready_.notify_all();
    };
    if (command == BF_BC_DEAD_BINDER_DONE) {
        uint64_t cookie; memcpy(&cookie, bytes, 8);
        auto it = std::find_if(deaths_.begin(), deaths_.end(), [&](const auto& d) {
            return d->delivered && d->cookie == cookie;
        });
        if (it == deaths_.end()) return -EINVAL;
        auto death = *it; death->delivered = false; death->acknowledged = true;
        if (death->clearing) clear_done(death);
        return 0;
    }
    bf_binder_handle_cookie value; memcpy(&value, bytes, sizeof(value));
    if (!value.handle && !handles_.count(0) && domain_->manager) {
        auto& object = domain_->manager->owned_objects_[domain_->manager_ptr];
        if (!object) {
            object = std::make_shared<Object>(); object->owner = domain_->manager->weak_from_this();
            object->pointer = domain_->manager_ptr; object->cookie = domain_->manager_cookie;
        }
        Handle handle; handle.object = object; handle.weak = 1; ++object->weak;
        handles_.emplace(0, std::move(handle));
    }
    auto it = handles_.find(value.handle);
    if (it == handles_.end()) return -EINVAL;
    auto& handle = it->second;
    if (!(handle.strong || handle.weak || handle.temporary_strong || handle.temporary_weak)) return -EINVAL;
    if (command == BF_BC_REQUEST_DEATH_NOTIFICATION) {
        if (handle.death) return -EINVAL;
        if (deaths_.size() >= MAX_OBJECTS) return -ENOMEM;
        auto death = std::make_shared<BinderDeath>();
        death->recipient = weak_from_this(); death->cookie = value.cookie;
        handle.death = death; deaths_.push_back(death);
        auto& watchers = handle.object->deaths;
        watchers.erase(std::remove_if(watchers.begin(), watchers.end(), [](const auto& w) { return w.expired(); }), watchers.end());
        watchers.push_back(death);
        if (!handle.object->alive || handle.object->owner.expired()) notify_death(death);
    } else {
        auto death = handle.death;
        if (!death || death->cookie != value.cookie) return -EINVAL;
        handle.death.reset(); death->clearing = true;
        if (!death->queued) { death->cancelled = true; clear_done(death); }
        // A delivered notification remains outstanding until DEAD_BINDER_DONE.
        else if (death->acknowledged) clear_done(death);
    }
    return 0;
}
void BinderNode::close_buffer_fds(uint64_t address) {
    auto found = buffer_fds_.find(address);
    if (found == buffer_fds_.end()) return;
    for (auto& file : found->second) {
        // Broker delivery may already have transferred and closed this entry.
        // Never close a subsequently reused descriptor number.
        if (file.table->get(file.number) == file.file.lock()) file.table->close(file.number);
    }
    buffer_fds_.erase(found);
}
void BinderNode::change_reference(const std::shared_ptr<Object>& object, int strong, int weak,
                                 int owner_tid) {
    bool had_weak = object->strong || object->weak, had_strong = object->strong != 0;
    object->strong += strong; object->weak += weak;
    auto owner_node = domain_->promote(object->owner);
    if (!owner_node || !object->alive) return;
    auto owner = static_cast<BinderNode*>(owner_node.get());
    auto send = [&](uint32_t command) {
        Event event(command); event.object = {object->pointer, object->cookie};
        // Linux binder_translate_binder queues initial node work on the
        // publishing thread's todo list. A pool thread must not race Parcel
        // recycling after BR_REPLY/BR_TRANSACTION_COMPLETE on that thread.
        auto& queue = owner_tid ? owner->replies_[owner_tid] : owner->incoming_;
        queue.push_back(std::move(event));
    };
    bool has_weak = object->strong || object->weak, has_strong = object->strong != 0;
    if (!had_weak && has_weak) { ++object->pending_weak; send(BF_BR_INCREFS); }
    if (!had_strong && has_strong) { ++object->pending_strong; send(BF_BR_ACQUIRE); }
    if (had_strong && !has_strong) send(BF_BR_RELEASE);
    if (had_weak && !has_weak) send(BF_BR_DECREFS);
    owner->ready_.notify_all();
}
int BinderNode::reference_command(uint32_t command, const uint8_t* bytes) {
    if (command == BF_BC_INCREFS_DONE || command == BF_BC_ACQUIRE_DONE) {
        bf_binder_ptr_cookie value; memcpy(&value, bytes, sizeof(value));
        auto found = owned_objects_.find(value.ptr);
        if (found == owned_objects_.end() || found->second->cookie != value.cookie) return -EINVAL;
        uint32_t& pending = command == BF_BC_INCREFS_DONE ? found->second->pending_weak : found->second->pending_strong;
        if (!pending) return -EINVAL;
        --pending; return 0;
    }
    uint32_t id; memcpy(&id, bytes, 4);
    if (id == 0) return domain_->manager || domain_->service_manager ? 0 : -EINVAL;
    auto it = handles_.find(id);
    if (it == handles_.end()) return -EINVAL;
    auto& h = it->second;
    if (!(h.strong || h.weak || h.temporary_strong || h.temporary_weak)) return -EINVAL;
    bool weak = command == BF_BC_INCREFS || command == BF_BC_DECREFS;
    bool increment = command == BF_BC_INCREFS || command == BF_BC_ACQUIRE;
    uint32_t& count = weak ? h.weak : h.strong;
    if (!increment && !count) return -EINVAL;
    if (increment && (count >= 1000000 || h.object->strong >= 1000000 || h.object->weak >= 1000000)) return -EOVERFLOW;
    if (auto node = domain_->promote(h.object->owner)) {
        if (static_cast<BinderNode*>(node.get())->incoming_.size() + 2 > 256) return -EAGAIN;
    }
    if (increment) ++count; else --count;
    change_reference(h.object, weak ? 0 : increment ? 1 : -1, weak ? increment ? 1 : -1 : 0);
    prune_death(h);
    return 0;
}
void BinderNode::drop_buffer_references(uint64_t address) {
    auto it = buffer_references_.find(address);
    if (it == buffer_references_.end()) return;
    for (const auto& ref : it->second) {
        auto handle = handles_.find(ref.handle);
        if (handle == handles_.end()) continue;
        auto& count = ref.weak ? handle->second.temporary_weak : handle->second.temporary_strong;
        if (count) { --count; change_reference(ref.object, ref.weak ? 0 : -1, ref.weak ? -1 : 0); }
        prune_death(handle->second);
    }
    buffer_references_.erase(it);
}
void BinderNode::release_all_references() {
    auto closing = weak_from_this();
    for (auto it = domain_->services.begin(); it != domain_->services.end();) {
        const auto& owner = it->second.object->owner;
        // Compare weak ownership without temporarily promoting another peer.
        // Dropping that peer's last shared reference can reenter teardown and
        // erase registry entries while this iterator is still in use.
        bool same_owner = !owner.owner_before(closing) && !closing.owner_before(owner);
        if (owner.expired() || same_owner) {
            change_reference(it->second.object, -1, 0);
            it = domain_->services.erase(it);
        } else ++it;
    }
    for (auto& entry : owned_objects_) {
        entry.second->alive = false;
        for (const auto& watcher : entry.second->deaths)
            if (auto death = watcher.lock()) notify_death(death);
    }
    for (auto& death : deaths_) death->cancelled = true;
    deaths_.clear();
    while (!buffer_fds_.empty()) close_buffer_fds(buffer_fds_.begin()->first);
    for (auto& entry : handles_) {
        auto& h = entry.second;
        change_reference(h.object, -static_cast<int>(h.strong + h.temporary_strong),
                                  -static_cast<int>(h.weak + h.temporary_weak));
    }
    handles_.clear(); buffer_references_.clear();
}
int BinderNode::translate_objects(BinderNode& target, Event& event, Memory& mem,
                                   const bf_binder_transaction& transaction, int tid,
                                   FdTable& fds, bool accepts_fds, uint64_t buffers_size, const TransactionInput* input) {
    if ((transaction.offsets_size & 7) || transaction.offsets_size / 8 > MAX_PARCEL_OBJECTS)
        return -EINVAL;
    std::vector<uint64_t> offsets(transaction.offsets_size / 8);
    if (input) offsets = input->offsets;
    else if (transaction.offsets_size) mem.read(transaction.offsets, offsets.data(), transaction.offsets_size);
    struct Plan { uint64_t offset; std::shared_ptr<Object> object; bool weak; uint32_t handle; int owner_tid; };
    std::vector<Plan> plans;
    std::vector<Descriptor> descriptors;
    auto staged_owned = owned_objects_;
    auto staged_handles = target.handles_;
    uint32_t next = target.next_handle_;
    uint64_t aligned = (transaction.data_size + 7) & ~uint64_t(7);
    uint64_t scatter_begin = aligned + transaction.offsets_size;
    event.payload.resize(scatter_begin + buffers_size);
    if (!offsets.empty()) memcpy(event.payload.data() + aligned, offsets.data(), transaction.offsets_size);
    struct Buffer { bf_binder_buffer object; uint64_t copied; };
    std::map<uint64_t, Buffer> parents;
    uint64_t used = 0, last_object = UINT64_MAX, last_minimum = 0;
    auto valid_fixup = [&](uint64_t parent, uint64_t offset) {
        uint64_t last = last_object, minimum = last_minimum;
        while (last != parent) {
            auto it = parents.find(last);
            if (it == parents.end() || !(it->second.object.flags & BF_BINDER_BUFFER_FLAG_HAS_PARENT)) return false;
            minimum = it->second.object.parent_offset + 8;
            last = it->second.object.parent;
        }
        return offset >= minimum;
    };
    uint64_t previous_end = 0;
    for (size_t index = 0; index < offsets.size(); ++index) {
        uint64_t offset = offsets[index];
        if ((offset & 3) || offset < previous_end || offset > transaction.data_size ||
            sizeof(bf_flat_binder) > transaction.data_size - offset) return -EINVAL;
        uint32_t type; memcpy(&type, event.payload.data() + offset, 4);
        size_t object_size = type == BF_BINDER_TYPE_PTR ? sizeof(bf_binder_buffer) :
            type == BF_BINDER_TYPE_FDA ? sizeof(bf_binder_fd_array) : sizeof(bf_flat_binder);
        if (object_size > transaction.data_size - offset) return -EINVAL;
        previous_end = offset + object_size;
        if (type == BF_BINDER_TYPE_PTR) {
            bf_binder_buffer buffer; memcpy(&buffer, event.payload.data() + offset, sizeof(buffer));
            if (buffer.flags & ~BF_BINDER_BUFFER_FLAG_HAS_PARENT || buffer.length > buffers_size - used) return -EINVAL;
            uint64_t length = (buffer.length + 7) & ~uint64_t(7);
            if (length > buffers_size - used) return -EINVAL;
            uint64_t copied = scatter_begin + used;
            if (!input && buffer.length) mem.read(buffer.buffer, event.payload.data() + copied, buffer.length);
            event.fixups.push_back({offset + offsetof(bf_binder_buffer, buffer), copied});
            if (buffer.flags & BF_BINDER_BUFFER_FLAG_HAS_PARENT) {
                auto parent = parents.find(buffer.parent);
                if (parent == parents.end() || (buffer.parent_offset & 7) ||
                    buffer.parent_offset > parent->second.object.length ||
                    8 > parent->second.object.length - buffer.parent_offset ||
                    !valid_fixup(buffer.parent, buffer.parent_offset)) return -EINVAL;
                event.fixups.push_back({parent->second.copied + buffer.parent_offset, copied});
            }
            parents.emplace(index, Buffer{buffer, copied}); used += length;
            last_object = index; last_minimum = 0;
            continue;
        }
        if (type == BF_BINDER_TYPE_FDA) {
            bf_binder_fd_array array; memcpy(&array, event.payload.data() + offset, sizeof(array));
            auto parent = parents.find(array.parent);
            if (parent == parents.end() || (array.parent_offset & 3) ||
                array.parent_offset > parent->second.object.length ||
                array.num_fds > (parent->second.object.length - array.parent_offset) / 4 ||
                array.num_fds > 64 - descriptors.size() || !valid_fixup(array.parent, array.parent_offset)) return -EINVAL;
            if (array.num_fds && !accepts_fds) return -EPERM;
            for (uint64_t i = 0; i < array.num_fds; ++i) {
                uint64_t location = parent->second.copied + array.parent_offset + i * 4;
                int32_t number; memcpy(&number, event.payload.data() + location, 4);
                auto node = fds.get(number);
                if (!node) return -EBADF;
                if (std::dynamic_pointer_cast<BinderNode>(node)) return -EINVAL;
                descriptors.push_back({location, std::move(node), 4, true});
            }
            last_object = array.parent; last_minimum = array.parent_offset + array.num_fds * 4;
            continue;
        }
        bf_flat_binder flat; memcpy(&flat, event.payload.data() + offset, sizeof(flat));
        if (flat.type == BF_BINDER_TYPE_FD) {
            if (!accepts_fds) return -EPERM;
            auto node = fds.get(static_cast<int32_t>(flat.binder));
            if (!node) return -EBADF;
            // A reference to the sending Binder device would create a queue
            // ownership cycle. Device-fd transfer is not supported here.
            if (std::dynamic_pointer_cast<BinderNode>(node)) return -EINVAL;
            if (descriptors.size() >= 64) return -EINVAL;
            descriptors.push_back({offset + offsetof(bf_flat_binder, binder), std::move(node)});
            continue;
        }
        bool weak = flat.type == BF_BINDER_TYPE_WEAK_BINDER || flat.type == BF_BINDER_TYPE_WEAK_HANDLE;
        std::shared_ptr<Object> object;
        if (flat.type == BF_BINDER_TYPE_BINDER || flat.type == BF_BINDER_TYPE_WEAK_BINDER) {
            // Null Binder is legal; it has no transferable reference.
            if (!flat.binder && !flat.cookie) continue;
            auto found = staged_owned.find(flat.binder);
            if (found != staged_owned.end() && found->second->cookie == flat.cookie) object = found->second;
            else {
                if (found != staged_owned.end() && (found->second->strong || found->second->weak)) return -EINVAL;
                if (found == staged_owned.end() && staged_owned.size() >= MAX_OBJECTS) return -ENOMEM;
                object = std::make_shared<Object>();
                object->owner = weak_from_this(); object->pointer = flat.binder; object->cookie = flat.cookie;
                object->accepts_fds = flat.flags & BF_FLAT_BINDER_FLAG_ACCEPTS_FDS;
                staged_owned[flat.binder] = object;
            }
        } else if (flat.type == BF_BINDER_TYPE_HANDLE || flat.type == BF_BINDER_TYPE_WEAK_HANDLE) {
            auto found = handles_.find(static_cast<uint32_t>(flat.binder));
            if (flat.binder > UINT32_MAX || found == handles_.end()) return -EINVAL;
            const auto& handle = found->second;
            if (!(handle.strong || handle.weak || handle.temporary_strong || handle.temporary_weak)) return -EINVAL;
            object = handle.object;
        } else return -ENOTSUP;
        if (!object->alive || object->owner.expired()) return -ENOENT;
        // Conservative queue reservation bounds lifecycle events before committing.
        auto owner_node = domain_->promote(object->owner);
        if (!owner_node) return -ENOENT;
        auto owner = static_cast<BinderNode*>(owner_node.get());
        int owner_tid = (flat.type == BF_BINDER_TYPE_BINDER ||
                         flat.type == BF_BINDER_TYPE_WEAK_BINDER) ? tid : 0;
        const auto& queue = owner_tid ? owner->replies_[owner_tid] : owner->incoming_;
        if (queue.size() + 2 * offsets.size() + 2 > 256) return -ENOMEM;
        if (object->strong > 1000000 || object->weak > 1000000) return -EOVERFLOW;
        uint32_t id = 0;
        auto existing = std::find_if(staged_handles.begin(), staged_handles.end(),
            [&](const auto& entry) { return entry.second.object == object; });
        if (existing != staged_handles.end()) id = existing->first;
        else {
            if (staged_handles.size() >= MAX_OBJECTS || !next) return -ENOMEM;
            id = next++; Handle handle; handle.object = object; staged_handles[id] = handle;
        }
        plans.push_back({offset, object, weak, id, owner_tid});
    }
    // All source data and objects are validated before any references change.
    owned_objects_ = std::move(staged_owned);
    event.descriptors = std::move(descriptors);
    target.handles_ = std::move(staged_handles); target.next_handle_ = next;
    for (const auto& plan : plans) {
        bf_flat_binder flat; memcpy(&flat, event.payload.data() + plan.offset, sizeof(flat));
        auto owner = domain_->promote(plan.object->owner);
        if (owner.get() == &target) {
            flat.type = plan.weak ? BF_BINDER_TYPE_WEAK_BINDER : BF_BINDER_TYPE_BINDER;
            flat.binder = plan.object->pointer; flat.cookie = plan.object->cookie;
        } else {
            flat.type = plan.weak ? BF_BINDER_TYPE_WEAK_HANDLE : BF_BINDER_TYPE_HANDLE;
            flat.binder = plan.handle; flat.cookie = 0;
        }
        memcpy(event.payload.data() + plan.offset, &flat, sizeof(flat));
        auto& handle = target.handles_.at(plan.handle);
        if (plan.weak) ++handle.temporary_weak; else ++handle.temporary_strong;
        change_reference(plan.object, plan.weak ? 0 : 1, plan.weak ? 1 : 0, plan.owner_tid);
        event.references.push_back({plan.handle, plan.weak, plan.object});
    }
    return 0;
}
}
