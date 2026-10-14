#include "System/Android/Binder/BinderNode.hpp"
#include "System/Android/Binder/Parcel.hpp"
#include "System/Memory/Memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace arm64emu::yggdrasil {
int BinderDomain::report_process_state(int pid, int32_t state) {
    // States/adjustments are the supported managed lifecycle, not host Linux
    // oom_score_adj or a claim that native service processes are AMS apps.
    int32_t adjustment = state == 2 ? 0 : state == 10 ? 500 : state == 19 ? 900 : -1;
    if (adjustment < 0) return -EINVAL;
    Guard guard(*this);
    for (const auto* endpoint : endpoints) {
        const uint32_t uid = endpoint->sender_uid_;
        if (endpoint->sender_pid_ == pid && ((uid >= 10000 && uid <= 19999) ||
                                           (uid >= 99000 && uid <= 99999))) {
            processes[pid] = {uid, state, adjustment};
            return 0;
        }
    }
    return -ESRCH;
}

int BinderNode::process_info_transaction(const bf_binder_transaction& tx, Memory& mem, int tid) {
    constexpr const char* descriptor = "android.os.IProcessInfoService";
    constexpr uint32_t status_code = 0x08;
    if (!mapping_ || memory_ != &mem || (tx.flags & BF_TF_ONE_WAY) || replies_[tid].size() + 2 > 256) {
        fail(tid, BF_BR_FAILED_REPLY, EINVAL); return 0;
    }
    Event reply(BF_BR_REPLY);
    reply.transaction.code = tx.code;
    AndroidParcelWriter out;
    try {
        if (tx.offsets_size) throw std::invalid_argument("processinfo objects");
        std::vector<uint8_t> data(tx.data_size);
        if (!data.empty()) mem.read(tx.buffer, data.data(), data.size());
        AndroidParcelReader in(data);
        if (tx.code == 0x5f504e47) {
            in.finish(); out.write<int32_t>(0); reply.transaction.flags = status_code;
        } else if (tx.code == 0x5f4e5446) {
            in.finish(); out.ascii_string(descriptor);
        } else {
            in.read<int32_t>(); in.read<int32_t>();
            if (in.read<uint32_t>() != 0x53595354 || in.ascii_string(64) != descriptor)
                throw std::invalid_argument("processinfo token");
            if (tx.code != 1 && tx.code != 2) {
                out.write<int32_t>(-EBADMSG); reply.transaction.flags = status_code;
            } else {
                int32_t count = in.read<int32_t>();
                if (count < 0 || count > 4096) throw std::invalid_argument("processinfo count");
                std::vector<int32_t> pids(count);
                for (auto& pid : pids) pid = in.read<int32_t>();
                if (in.read<int32_t>() != count || (tx.code == 2 && in.read<int32_t>() != count))
                    throw std::invalid_argument("processinfo output lengths");
                in.finish();
                // API-35 platform.xml grants score queries to media and camera;
                // ordinary apps have no GET_PROCESS_STATE_AND_OOM_SCORE grant.
                if (tx.code == 2 && sender_uid_ != 0 && sender_uid_ != 1000 &&
                        sender_uid_ != 1013 && sender_uid_ != 1047) {
                    out.write<int32_t>(-1); // EX_SECURITY
                    out.ascii_string("Requires GET_PROCESS_STATE_AND_OOM_SCORE");
                    out.write<int32_t>(0); // remote stack trace header
                } else {
                    out.write<int32_t>(0); // no exception
                    out.write(count);
                    for (int pid : pids) {
                        auto process = domain_->processes.find(pid);
                        out.write<int32_t>(process == domain_->processes.end() ? 20 : process->second.state);
                    }
                    if (tx.code == 2) {
                        out.write(count);
                        for (int pid : pids) {
                            auto process = domain_->processes.find(pid);
                            out.write<int32_t>(process == domain_->processes.end() ? -10000 : process->second.adjustment);
                        }
                    }
                    out.write<int32_t>(0); // native IProcessInfoService trailing status
                }
            }
        }
    } catch (const std::invalid_argument&) {
        out.bytes.clear(); out.write<int32_t>(-EINVAL); reply.transaction.flags = status_code;
    }
    uint64_t free_bytes = 0;
    for (const auto& range : free_) free_bytes += range.second;
    const uint64_t size = std::max<uint64_t>(8, (out.bytes.size() + 7) & ~uint64_t(7));
    if (queued_bytes_ > free_bytes || size > free_bytes - queued_bytes_) {
        fail(tid, BF_BR_FAILED_REPLY, ENOMEM); return 0;
    }
    reply.transaction.data_size = out.bytes.size();
    reply.payload = std::move(out.bytes);
    queued_bytes_ += size;
    replies_[tid].push_back(Event{BF_BR_TRANSACTION_COMPLETE});
    replies_[tid].push_back(std::move(reply));
    ready_.notify_all();
    return 0;
}
}
