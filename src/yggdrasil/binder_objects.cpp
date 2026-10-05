#include "yggdrasil/binder_node.hpp"
#include "core/memory.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace arm64emu::yggdrasil {
namespace { constexpr size_t MAX_OBJECTS = 1024, MAX_PARCEL_OBJECTS = 64; }
void BinderNode::change_reference(const std::shared_ptr<Object>& object, int strong, int weak) {
    bool had_weak = object->strong || object->weak, had_strong = object->strong != 0;
    object->strong += strong; object->weak += weak;
    auto owner_node = object->owner.lock();
    if (!owner_node || !object->alive) return;
    auto owner = static_cast<BinderNode*>(owner_node.get());
    auto send = [&](uint32_t command) {
        Event event(command); event.object = {object->pointer, object->cookie};
        owner->incoming_.push_back(std::move(event));
    };
    bool has_weak = object->strong || object->weak, has_strong = object->strong != 0;
    if (!had_weak && has_weak) { ++object->pending_weak; send(BF_BR_INCREFS); }
    if (!had_strong && has_strong) { ++object->pending_strong; send(BF_BR_ACQUIRE); }
    if (had_strong && !has_strong) send(BF_BR_RELEASE);
    if (had_weak && !has_weak) send(BF_BR_DECREFS);
    domain_->ready.notify_all();
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
    if (auto node = h.object->owner.lock()) {
        if (static_cast<BinderNode*>(node.get())->incoming_.size() + 2 > 256) return -EAGAIN;
    }
    if (increment) ++count; else --count;
    change_reference(h.object, weak ? 0 : increment ? 1 : -1, weak ? increment ? 1 : -1 : 0);
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
    for (auto& entry : owned_objects_) entry.second->alive = false;
    for (auto& entry : handles_) {
        auto& h = entry.second;
        change_reference(h.object, -static_cast<int>(h.strong + h.temporary_strong),
                                  -static_cast<int>(h.weak + h.temporary_weak));
    }
    handles_.clear(); buffer_references_.clear();
}
int BinderNode::translate_objects(BinderNode& target, Event& event, Memory& mem,
                                   const bf_binder_transaction& transaction) {
    if (!transaction.offsets_size) return 0;
    if ((transaction.offsets_size & 7) || transaction.offsets_size / 8 > MAX_PARCEL_OBJECTS)
        return -EINVAL;
    std::vector<uint64_t> offsets(transaction.offsets_size / 8);
    mem.read(transaction.offsets, offsets.data(), transaction.offsets_size);
    struct Plan { uint64_t offset; std::shared_ptr<Object> object; bool weak; uint32_t handle; };
    std::vector<Plan> plans;
    auto staged_owned = owned_objects_;
    auto staged_handles = target.handles_;
    uint32_t next = target.next_handle_;
    uint64_t previous_end = 0;
    for (uint64_t offset : offsets) {
        if ((offset & 3) || offset < previous_end || offset > transaction.data_size ||
            sizeof(bf_flat_binder) > transaction.data_size - offset) return -EINVAL;
        previous_end = offset + sizeof(bf_flat_binder);
        bf_flat_binder flat; memcpy(&flat, event.payload.data() + offset, sizeof(flat));
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
                staged_owned[flat.binder] = object;
            }
        } else if (flat.type == BF_BINDER_TYPE_HANDLE || flat.type == BF_BINDER_TYPE_WEAK_HANDLE) {
            auto found = handles_.find(static_cast<uint32_t>(flat.binder));
            if (flat.binder > UINT32_MAX || found == handles_.end()) return -EINVAL;
            const auto& handle = found->second;
            if (!(handle.strong || handle.weak || handle.temporary_strong || handle.temporary_weak)) return -EINVAL;
            object = handle.object;
        } else return -ENOTSUP; // FD/PTR/FDA need separate descriptor/fixup support.
        if (!object->alive || object->owner.expired()) return -ENOENT;
        // Conservative queue reservation bounds lifecycle events before committing.
        auto owner_node = object->owner.lock();
        auto owner = static_cast<BinderNode*>(owner_node.get());
        if (owner->incoming_.size() + 2 * offsets.size() + 1 > 256) return -ENOMEM;
        if (object->strong > 1000000 || object->weak > 1000000) return -EOVERFLOW;
        uint32_t id = 0;
        auto existing = std::find_if(staged_handles.begin(), staged_handles.end(),
            [&](const auto& entry) { return entry.second.object == object; });
        if (existing != staged_handles.end()) id = existing->first;
        else {
            if (staged_handles.size() >= MAX_OBJECTS || !next) return -ENOMEM;
            id = next++; Handle handle; handle.object = object; staged_handles[id] = handle;
        }
        plans.push_back({offset, object, weak, id});
    }
    // All source data and objects are validated before any references change.
    owned_objects_ = std::move(staged_owned);
    target.handles_ = std::move(staged_handles); target.next_handle_ = next;
    for (const auto& plan : plans) {
        bf_flat_binder flat; memcpy(&flat, event.payload.data() + plan.offset, sizeof(flat));
        auto owner = plan.object->owner.lock();
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
        change_reference(plan.object, plan.weak ? 0 : 1, plan.weak ? 1 : 0);
        event.references.push_back({plan.handle, plan.weak, plan.object});
    }
    uint64_t aligned = (transaction.data_size + 7) & ~uint64_t(7);
    event.payload.resize(aligned + transaction.offsets_size);
    memcpy(event.payload.data() + aligned, offsets.data(), transaction.offsets_size);
    return 0;
}
}
