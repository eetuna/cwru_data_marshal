/*
 * File: src/slice_agent_client.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: TCP client for the scanner-side `slice_agent --listen` server.
 *
 * Protocol (dynamic-slice-position-main/agent/PROTOCOL.md, verified against
 * slice_agent.cpp):
 *   - TCP, default port 9270, ONE client at a time, no framing, no replies.
 *   - Each message is a 56-byte little-endian SliceCommand (slice_math.hpp).
 *   - The agent publishes an IDENTITY transform to shared memory on every
 *     client connect and again on disconnect. Identity = axial slice at
 *     isocenter, NOT the prescribed slice. Therefore:
 *       * we never connect until we have a real command to send, and
 *       * right after connecting we re-send the last command repeatedly for
 *         a short window so the identity publish is overwritten before the
 *         sequence's next frame read.
 *   - The agent restarts its frame counter at 0 per connection while the
 *     sequence only accepts frames >= the last one it saw, so after a
 *     reconnect the sequence may ignore commands for a while. We keep the
 *     connection open for the whole session and log loudly on reconnect.
 *
 * Threading: the caller (HTTP handler) only touches a mailbox; a single
 * worker thread owns the socket, connects lazily, writes, reconnects with
 * backoff, and sends flags=0xDEAD on stop(). submit() never blocks on I/O.
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "slice_agent"
#include "logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "slice_math.hpp"

namespace mrd {

struct SliceAgentConfig {
    std::string host;                 // empty = disabled
    uint16_t    port{9270};
    uint32_t    connect_timeout_ms{3000};
    uint32_t    resend_window_ms{2000};   // re-send last cmd after (re)connect
    uint32_t    resend_interval_ms{100};
    uint32_t    backoff_min_ms{500};
    uint32_t    backoff_max_ms{10000};
};

class SliceAgentClient {
public:
    explicit SliceAgentClient(SliceAgentConfig cfg) : cfg_(std::move(cfg)) {}
    ~SliceAgentClient() { stop(); }

    SliceAgentClient(const SliceAgentClient&) = delete;
    SliceAgentClient& operator=(const SliceAgentClient&) = delete;

    bool enabled() const { return !cfg_.host.empty(); }

    // Start the worker. Does NOT connect — the first submit() does.
    void start() {
        if (!enabled()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (worker_.joinable()) return;
        stopping_ = false;
        worker_ = std::thread([this] { run(); });
    }

    // Queue a command and wait (bounded) for the worker to write it. Returns
    // true once the bytes were handed to the agent's socket; false when
    // disabled, stopped, or the write did not happen within wait_ms (no
    // agent reachable). The command is kept either way and (re)sent as soon
    // as a connection exists. Never blocks on I/O itself — only waits on the
    // worker's progress — so an unreachable agent costs at most wait_ms.
    bool submit(const slice_math::WireCommand& cmd, uint32_t wait_ms = 1500) {
        if (!enabled()) return false;
        uint64_t my_gen;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return false;
            last_cmd_ = cmd;
            my_gen = ++generation_;
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lk(mtx_);
        ack_cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] {
            return stopping_ || sent_gen_ack_ >= my_gen || failed_gen_ack_ >= my_gen;
        });
        return sent_gen_ack_ >= my_gen;
    }

    bool connected() const { return connected_.load(); }
    uint64_t sent_count() const { return sent_count_.load(); }
    uint32_t reconnect_count() const { return reconnects_.load(); }

    std::optional<slice_math::WireCommand> last_command() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_cmd_;
    }

    // Send 0xDEAD if connected, close, join. Idempotent.
    void stop() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!worker_.joinable()) { stopping_ = true; return; }
            stopping_ = true;
            t = std::move(worker_);
        }
        cv_.notify_all();
        ack_cv_.notify_all();
        // Unblock a worker stuck in a blocking send: shut the fd down.
        {
            std::lock_guard<std::mutex> lk(fd_mtx_);
            if (fd_ >= 0) ::shutdown(fd_, SHUT_RD);
        }
        if (t.joinable()) t.join();
    }

private:
    // ---- worker -------------------------------------------------------------

    void run() {
        uint64_t sent_gen = 0;
        uint32_t backoff_ms = cfg_.backoff_min_ms;

        while (true) {
            slice_math::WireCommand cmd;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                // Wake periodically to notice a peer that went away while we
                // were idle (the agent never sends, so a plain blocking wait
                // would only learn about it at the next failed send).
                cv_.wait_for(lk, std::chrono::milliseconds(kLivenessPollMs),
                             [&] { return stopping_ || (last_cmd_ && generation_ > sent_gen); });
                if (stopping_) break;
                if (!last_cmd_) continue;
                if (generation_ <= sent_gen) {
                    // Idle tick: liveness check only.
                    lk.unlock();
                    if (connected_.load() && peer_closed()) {
                        on_send_failure();
                        sent_gen = 0;   // re-send the last command after reconnect
                    }
                    continue;
                }
                cmd = *last_cmd_;
                sent_gen = generation_;
            }

            if (connected_.load() && peer_closed()) {
                on_send_failure();
            }

            if (!connected_.load()) {
                if (!connect_once()) {
                    // Keep the command; retry after backoff unless stopped.
                    ack(failed_gen_ack_, sent_gen);
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait_for(lk, std::chrono::milliseconds(backoff_ms),
                                 [&] { return stopping_; });
                    if (stopping_) break;
                    backoff_ms = std::min(backoff_ms * 2, cfg_.backoff_max_ms);
                    sent_gen = 0;   // force a resend attempt of the same command
                    continue;
                }
                backoff_ms = cfg_.backoff_min_ms;
                // Overwrite the agent's identity publish quickly and keep
                // repeating for the resend window (the sequence reads shm
                // once per image frame and may ignore early frames after a
                // reconnect — see header comment).
                resend_window(cmd, sent_gen);
                continue;
            }

            if (send_cmd(cmd)) {
                ack(sent_gen_ack_, sent_gen);
            } else {
                on_send_failure();
                ack(failed_gen_ack_, sent_gen);
                sent_gen = 0;   // resend after reconnect
            }
        }

        // Graceful quit: tell the agent we are leaving, then close.
        if (connected_.load()) {
            slice_math::WireCommand quit;
            quit.flags = slice_math::kFlagQuit;
            send_cmd(quit);
        }
        close_fd();
    }

    // Re-send `cmd` immediately and then every resend_interval_ms for
    // resend_window_ms, unless a newer command arrives (then it takes over)
    // or stop() is called. Returns false on socket failure.
    bool resend_window(slice_math::WireCommand cmd, uint64_t& sent_gen) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(cfg_.resend_window_ms);
        while (true) {
            if (!send_cmd(cmd)) {
                on_send_failure();
                ack(failed_gen_ack_, sent_gen);
                sent_gen = 0;
                return false;
            }
            ack(sent_gen_ack_, sent_gen);
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.resend_interval_ms),
                         [&] { return stopping_ || generation_ > sent_gen; });
            if (stopping_) return false;
            if (generation_ > sent_gen) {
                cmd = *last_cmd_;
                sent_gen = generation_;
                // newer command: restart the window for it
                deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(cfg_.resend_window_ms);
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) return true;
        }
    }

    // Record worker progress for submit()'s bounded wait.
    void ack(uint64_t& slot, uint64_t gen) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (gen > slot) slot = gen;
        }
        ack_cv_.notify_all();
    }

    void on_send_failure() {
        const bool was_connected = connected_.exchange(false);
        close_fd();
        if (was_connected) {
            reconnects_.fetch_add(1);
            LOG_WARN("slice_agent connection lost; will reconnect. NOTE: the "
                     "scanner sequence may ignore commands after a reconnect "
                     "until the agent's frame counter catches up.");
        }
    }

    // ---- socket -------------------------------------------------------------

    bool connect_once() {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(cfg_.port);
        const int rc = ::getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 || !res) {
            LOG_WARN("slice_agent: resolve " << cfg_.host << " failed: " << ::gai_strerror(rc));
            return false;
        }

        int fd = -1;
        bool ok = false;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (fd < 0) continue;
            ok = connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen);
            if (ok) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);
        if (!ok) {
            LOG_WARN("slice_agent: connect to " << cfg_.host << ":" << cfg_.port
                     << " failed (" << std::strerror(errno) << ")");
            return false;
        }

        configure_socket(fd);
        {
            std::lock_guard<std::mutex> lk(fd_mtx_);
            fd_ = fd;
        }
        connected_.store(true);
        LOG_INFO("slice_agent: connected to " << cfg_.host << ":" << cfg_.port);
        return true;
    }

    bool connect_with_timeout(int fd, const sockaddr* addr, socklen_t len) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, addr, len);
        if (rc != 0 && errno != EINPROGRESS) return false;
        if (rc != 0) {
            pollfd p{fd, POLLOUT, 0};
            rc = ::poll(&p, 1, static_cast<int>(cfg_.connect_timeout_ms));
            if (rc <= 0) { errno = (rc == 0) ? ETIMEDOUT : errno; return false; }
            int err = 0;
            socklen_t elen = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
                errno = err ? err : errno;
                return false;
            }
        }
        ::fcntl(fd, F_SETFL, flags);   // back to blocking for send()
        return true;
    }

    static void configure_socket(int fd) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        int v = 30; ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
        v = 10;     ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
        v = 3;      ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
        unsigned ut = 60000; ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof(ut));
    }

    // The agent never sends anything, so any readable byte or EOF means the
    // peer has gone away (EOF) or is misbehaving. Non-blocking peek.
    bool peer_closed() {
        int fd;
        {
            std::lock_guard<std::mutex> lk(fd_mtx_);
            fd = fd_;
        }
        if (fd < 0) return true;
        char byte;
        const ssize_t n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return true;                       // orderly close
        if (n < 0) return !(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        return false;                                   // unexpected data; keep going
    }

    static constexpr uint32_t kLivenessPollMs = 1000;

    bool send_cmd(const slice_math::WireCommand& cmd) {
        int fd;
        {
            std::lock_guard<std::mutex> lk(fd_mtx_);
            fd = fd_;
        }
        if (fd < 0) return false;
        const auto bytes = slice_math::to_wire(cmd);
        size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            off += static_cast<size_t>(n);
        }
        sent_count_.fetch_add(1);
        return true;
    }

    void close_fd() {
        std::lock_guard<std::mutex> lk(fd_mtx_);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        connected_.store(false);
    }

    SliceAgentConfig cfg_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;       // worker wake-ups
    std::condition_variable ack_cv_;   // submit() waiters
    bool stopping_{false};
    uint64_t generation_{0};
    uint64_t sent_gen_ack_{0};         // highest generation written to the socket
    uint64_t failed_gen_ack_{0};       // highest generation whose send/connect failed
    std::optional<slice_math::WireCommand> last_cmd_;
    std::thread worker_;

    std::mutex fd_mtx_;
    int fd_{-1};

    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint32_t> reconnects_{0};
};

} // namespace mrd
