#include "System/Android/Media/CodecServices.hpp"
#include "System/Android/Media/HostDecoder.hpp"
#include "System/Android/Binder/BinderNode.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <vector>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;

namespace arm64emu::yggdrasil {
struct CodecServices::Impl {
    using Clock = std::chrono::steady_clock;
    struct Service {
        const char* role;
        const char* name;
        const char* uid;
        pid_t pid = -1;
        unsigned restarts = 0;
        int last_exit = 0;
        Clock::time_point started{}, retry{};
    };
    std::array<Service, 3> services{{
        {"resource", "media.resource_manager", "1013"},
        {"host", "android.hardware.media.c2.IComponentStore/default", "1046"},
        {"software", "android.hardware.media.c2.IComponentStore/software", "1046"}}};
    std::string socket, root, executable, host_capabilities;
    std::shared_ptr<BinderDomain> domain;
    unsigned mask;
    std::mutex mutex, stop_mutex;
    std::atomic<bool> stopped{false};
    bool worker_failed = false;
    std::thread worker;

    Impl(std::string socket, std::string root, unsigned mask, std::shared_ptr<BinderDomain> domain)
        : socket(std::move(socket)), root(std::move(root)), domain(std::move(domain)), mask(mask) {
        // Resolve through the broker's live executable, including after an
        // on-disk upgrade has replaced or unlinked the original pathname.
        executable = "/proc/self/exe";
        host_capabilities = HostDecoder::capabilities_json();
        worker = std::thread([this] {
            try { supervise(); }
            catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                worker_failed = true;
            }
            cleanup();
        });
    }
    int spawn(Service& service) {
        std::vector<std::string> arguments{executable, "--asgard-service-parent", std::to_string(::getpid()),
            "--android-runtime", "--android-services", "--android-system-uid", service.uid,
            "--android-binder-broker", socket, "--rootfs", root,
            std::string(service.role) == "resource" ? "/system/bin/bifrost-media-resource"
                : "/apex/com.android.media.swcodec/bin/mediaswcodec"};
        std::vector<std::string> environment;
        for (char** entry = environ; *entry; ++entry)
            if (std::strncmp(*entry, "BIFROST_GUEST_ENV=", 18) &&
                    std::strncmp(*entry, "BIFROST_JIT_VERIFY=", 19)) environment.emplace_back(*entry);
        std::string guest = "BIFROST_GUEST_ENV=ANDROID_ROOT=/system;ANDROID_DATA=/data;"
            "ANDROID_ART_ROOT=/apex/com.android.art;ANDROID_I18N_ROOT=/apex/com.android.i18n;"
            "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata;LD_PRELOAD=/system/lib64/libbifrost-media-log.so";
        if (std::string(service.role) == "host") guest += ":/system/lib64/libbifrost-codec2-store.so";
        environment.push_back(std::move(guest));
        std::vector<char*> argv, envp;
        for (auto& value : arguments) argv.push_back(value.data());
        for (auto& value : environment) envp.push_back(value.data());
        argv.push_back(nullptr); envp.push_back(nullptr);
        const std::string log_path = socket + ".media-" + service.role + ".log";
        int log = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (log < 0) return errno;
        posix_spawn_file_actions_t actions;
        int error = ::posix_spawn_file_actions_init(&actions);
        if (!error) {
            error = ::posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
            if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
            if (!error) error = ::posix_spawn(&service.pid, executable.c_str(), &actions, nullptr, argv.data(), envp.data());
            ::posix_spawn_file_actions_destroy(&actions);
        }
        ::close(log);
        return error;
    }
    bool registered(const Service& service) {
        BinderDomain::Guard guard(*domain);
        auto found = domain->services.find(service.name);
        if (found == domain->services.end() || !found->second.object->reachable()) return false;
        auto owner = domain->promote(found->second.object->owner);
        return service.pid > 0 && owner && static_cast<BinderNode*>(owner.get())->sender_pid() == service.pid;
    }
    void supervise() {
        while (!stopped.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto now = Clock::now();
                for (auto& service : services) {
                    if (!mask && std::string(service.role) == "host") continue;
                    if (service.pid > 0) {
                        int status = 0;
                        pid_t exited = ::waitpid(service.pid, &status, WNOHANG);
                        if (exited == service.pid || (exited < 0 && errno == ECHILD)) {
                            service.pid = -1;
                            service.last_exit = WIFEXITED(status) ? WEXITSTATUS(status) :
                                WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
                            ++service.restarts;
                            service.retry = now + std::chrono::milliseconds(250 * service.restarts);
                        } else if (exited == 0) {
                            if (now - service.started > std::chrono::seconds(30) && !registered(service)) {
                                // A live process is insufficient: it must publish its own endpoint.
                                ::kill(service.pid, SIGKILL);
                            } else if (now - service.started > std::chrono::seconds(60)) service.restarts = 0;
                        }
                    }
                    if (service.pid <= 0 && service.restarts < 4 && now >= service.retry) {
                        int error = spawn(service);
                        if (error) {
                            service.pid = -1; service.last_exit = -error; ++service.restarts;
                            service.retry = now + std::chrono::seconds(1);
                        } else service.started = now;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    static pid_t reap(pid_t pid, int flags) {
        pid_t result;
        do { result = ::waitpid(pid, nullptr, flags); } while (result < 0 && errno == EINTR);
        return result;
    }
    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& service : services) if (service.pid > 0) ::kill(service.pid, SIGTERM);
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        for (auto& service : services) if (service.pid > 0) {
            pid_t result = reap(service.pid, WNOHANG);
            while (result == 0 && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                result = reap(service.pid, WNOHANG);
            }
            // Kill only the still-owned, unreaped child; never a reused PID.
            if (result == 0) {
                ::kill(service.pid, SIGKILL);
                reap(service.pid, 0);
            }
            service.pid = -1;
        }
    }
    bool ready_locked() {
        if (stopped.load() || worker_failed) return false;
        for (const auto& service : services) {
            if (!mask && std::string(service.role) == "host") continue;
            if (service.pid <= 0 || !registered(service)) return false;
        }
        return true;
    }
};
CodecServices::CodecServices(std::string socket, std::string root, unsigned mask,
        std::shared_ptr<BinderDomain> domain)
    : impl_(std::make_unique<Impl>(std::move(socket), std::move(root), mask, std::move(domain))) {}
CodecServices::~CodecServices() { stop(); }
void CodecServices::stop() {
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->stopped.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
}
bool CodecServices::ready() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ready_locked();
}
std::string CodecServices::status() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    bool failed = impl_->worker_failed;
    for (const auto& service : impl_->services)
        if (service.pid <= 0 && service.restarts >= 4) failed = true;
    std::string out = "{\"state\":\"" + std::string(impl_->ready_locked() ? "ready" :
        impl_->stopped.load() ? "stopped" : failed ? "failed" : "starting") + "\",\"routing_mask\":" + std::to_string(impl_->mask)
        + ",\"host\":" + impl_->host_capabilities + ",\"services\":[";
    bool first = true;
    for (const auto& service : impl_->services) {
        if (!impl_->mask && std::string(service.role) == "host") continue;
        if (!first) out += ',';
        first = false;
        out += "{\"name\":\"" + std::string(service.name) + "\",\"pid\":" + std::to_string(service.pid)
            + ",\"registered\":" + (impl_->registered(service) ? "true" : "false")
            + ",\"restarts\":" + std::to_string(service.restarts)
            + ",\"last_exit\":" + std::to_string(service.last_exit) + "}";
    }
    return out + "]}";
}
}
