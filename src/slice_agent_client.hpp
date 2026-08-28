/*
 * File: src/slice_agent_client.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: TCP client for the scanner-side `slice_agent --listen` server.
 *
 * Protocol (dynamic-slice-position-main/agent/PROTOCOL.md, verified against
 * slice_agent.cpp):
 *   - TCP, default port 9270, ONE client at a time, no framing, no replies.
 *   - Each message is a 56-byte little-endian SliceCommand (slice_math.hpp)
 *     carrying six ABSOLUTE numbers.
 *   - The agent publishes an identity transform on every client connect and
 *     again on disconnect. We therefore never connect idle (only when a
 *     command exists) and re-send the current command a few times right
 *     after each connect so the identity publish is overwritten before the
 *     sequence's next frame read.
 *   - The agent restarts its frame counter per connection while the sequence
 *     only accepts frames >= the last one it saw, so after a reconnect the
 *     sequence may ignore commands for a while. We hold the connection for
 *     the whole session, reconnect automatically if it drops, log loudly and
 *     count reconnects.
 *
 * Threading: callers only touch a mailbox; one worker thread owns the socket
 * (connect, write, liveness, reconnect with backoff, 0xDEAD on stop).
 *   post(cmd)  -> generation   (never blocks; safe under any caller lock)
 *   wait(gen)  -> delivered?   (bounded wait for the worker's verdict)
 *   submit(cmd) = post + wait.
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
#include <set>
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
    uint32_t    connect_timeout_ms{2000};
    uint32_t    resend_window_ms{2000};   // re-send after (re)connect for this long
    uint32_t    resend_interval_ms{100};
    uint32_t    backoff_min_ms{500};
    uint32_t    backoff_max_ms{10000};
    uint32_t    liveness_poll_ms{1000};
};

class SliceAgentClient {
public:
    explicit SliceAgentClient(SliceAgentConfig cfg) : cfg_(std::move(cfg)) {}
    ~SliceAgentClient() { stop(); }

    SliceAgentClient(const SliceAgentClient&) = delete;
    SliceAgentClient& operator=(const SliceAgentClient&) = delete;

    bool enabled() const { return !cfg_.host.empty(); }

    // Start the worker. Does NOT connect — the first command does.
    void start() {
        if (!enabled()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (worker_.joinable()) return;
        stopping_ = false;
        worker_done_ = false;
        worker_ = std::thread([this] { run(); });
    }

    // Queue a command; returns its generation (0 = disabled/stopped). Never
    // blocks, never touches the socket — safe to call under a caller's lock,
    // which is how ordering across HTTP threads is guaranteed.
    uint64_t post(const slice_math::WireCommand& cmd) {
        if (!enabled()) return 0;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return 0;
            last_cmd_ = cmd;
            gen = ++generation_;
        }
        cv_.notify_all();
        return gen;
    }

    // Wait (bounded by connect timeout + margin) for the worker's verdict on
    // generation `gen`: true once the bytes were written to the agent's
    // socket; false if the write did not happen in time (agent unreachable —
    // the command is kept and sent as soon as a connection exists), or if
    // disabled/stopped. A refused connection returns immediately.
    bool wait(uint64_t gen) { return verdict(gen) == slice_math::Delivery::Delivered; }

    // Exact per-generation verdict. Only a generation the worker actually
    // wrote counts as Delivered; ack >= gen alone is NOT proof, because the
    // worker always sends the newest posted command and skips any older
    // ones posted in between (those are Superseded).
    slice_math::Delivery verdict(uint64_t gen) {
        if (gen == 0) return slice_math::Delivery::NotDelivered;
        std::unique_lock<std::mutex> lk(mtx_);
        const auto budget = std::chrono::milliseconds(cfg_.connect_timeout_ms + 500);
        cv_.wait_for(lk, budget, [&] {
            return stopping_ || sent_gen_ack_ >= gen || failed_gen_ack_ >= gen;
        });
        if (sent_gens_.count(gen)) return slice_math::Delivery::Delivered;
        if (sent_gen_ack_ >= gen) return slice_math::Delivery::Superseded;
        return slice_math::Delivery::NotDelivered;
    }

    bool submit(const slice_math::WireCommand& cmd) { return wait(post(cmd)); }

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
        // Give the worker a moment to send the graceful 0xDEAD and exit on
        // its own; only if it is stuck (blocking send() to a hung agent, or
        // a connect in progress) force the socket shut — SHUT_RDWR, since
        // SHUT_RD alone does not wake a blocked writer.
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(kStopGraceMs), [&] { return worker_done_; });
            if (!worker_done_) {
                std::lock_guard<std::mutex> flk(fd_mtx_);
                if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
            }
        }
        if (t.joinable()) t.join();
    }

    static constexpr uint32_t kStopGraceMs = 500;

private:
    // ---- worker -------------------------------------------------------------

    void run() {
        uint64_t failed_gen = 0;        // generation whose connect attempt last failed
        uint32_t backoff_ms = cfg_.backoff_min_ms;
        auto next_attempt = std::chrono::steady_clock::now();

        while (true) {
            slice_math::WireCommand cmd;
            uint64_t gen;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                auto pending = [&] { return last_cmd_ && generation_ > sent_gen_; };
                if (!pending()) {
                    // Idle: wake on stop, on a new command, or periodically
                    // for a liveness peek while connected.
                    if (connected_.load())
                        cv_.wait_for(lk, std::chrono::milliseconds(cfg_.liveness_poll_ms),
                                     [&] { return stopping_ || pending(); });
                    else
                        cv_.wait(lk, [&] { return stopping_ || pending(); });
                    if (stopping_) break;
                    if (!pending()) {
                        lk.unlock();
                        if (connected_.load() && peer_closed()) {
                            // Dropped while idle: reconnect and re-send the
                            // current command so the agent's identity publish
                            // (on disconnect) does not stand.
                            on_send_failure();
                            std::lock_guard<std::mutex> lk2(mtx_);
                            if (last_cmd_) sent_gen_ = 0;   // mark for resend
                        }
                        continue;
                    }
                }
                cmd = *last_cmd_;
                gen = generation_;
            }

            if (connected_.load() && peer_closed()) on_send_failure();

            if (!connected_.load()) {
                // Automatic retries of the SAME command respect the backoff;
                // a NEW command (a user action) is attempted immediately —
                // the agent may well be back.
                const auto now = std::chrono::steady_clock::now();
                if (gen == failed_gen && now < next_attempt) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait_until(lk, next_attempt, [&] { return stopping_ || generation_ > gen; });
                    if (stopping_) break;
                    continue;
                }
                if (!connect_once()) {
                    if (stopping_) break;
                    failed_gen = gen;
                    ack(failed_gen_ack_, gen);
                    next_attempt = std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds(backoff_ms);
                    backoff_ms = std::min(backoff_ms * 2, cfg_.backoff_max_ms);
                    continue;
                }
                backoff_ms = cfg_.backoff_min_ms;
                resend_window(cmd, gen);
                continue;
            }

            if (send_cmd(cmd)) {
                mark_sent(gen);
            } else {
                on_send_failure();
                ack(failed_gen_ack_, gen);
                // sent_gen_ unchanged (< generation_): retried after reconnect.
            }
        }

        // Graceful quit: tell the agent we are leaving, then close.
        if (connected_.load()) {
            slice_math::WireCommand quit;
            quit.flags = slice_math::kFlagQuit;
            send_cmd(quit);
        }
        close_fd();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            worker_done_ = true;
        }
        cv_.notify_all();
    }

    // Right after a (re)connect: send `cmd` now and repeat it every
    // resend_interval_ms for resend_window_ms so the agent's identity publish
    // is overwritten before the sequence's next frame. A newer command
    // arriving inside the window ends it (the main loop sends the new one).
    void resend_window(const slice_math::WireCommand& cmd, uint64_t gen) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(cfg_.resend_window_ms);
        bool first = true;
        while (true) {
            if (!send_cmd(cmd)) {
                on_send_failure();
                ack(failed_gen_ack_, gen);
                return;
            }
            if (first) { mark_sent(gen); first = false; }
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.resend_interval_ms),
                         [&] { return stopping_ || generation_ > gen; });
            if (stopping_ || generation_ > gen) return;
            if (std::chrono::steady_clock::now() >= deadline) return;
        }
    }

    void mark_sent(uint64_t gen) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (gen > sent_gen_) sent_gen_ = gen;
            if (gen > sent_gen_ack_) sent_gen_ack_ = gen;
            sent_gens_.insert(gen);
            // Bounded: waiters only ever ask about recent generations.
            while (sent_gens_.size() > kSentHistory) sent_gens_.erase(sent_gens_.begin());
        }
        cv_.notify_all();
    }

    void ack(uint64_t& slot, uint64_t gen) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (gen > slot) slot = gen;
        }
        cv_.notify_all();
    }

    void on_send_failure() {
        const bool was_connected = connected_.exchange(false);
        close_fd();
        if (was_connected) {
            reconnects_.fetch_add(1);
            LOG_WARN("slice_agent connection lost; reconnecting. NOTE: the scanner "
                     "sequence may ignore commands after a reconnect until the "
                     "agent's frame counter catches up.");
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

        bool ok = false;
        int last_err = 0;
        for (addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
            const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (fd < 0) { last_err = errno; continue; }
            // Publish the in-progress fd so stop() can interrupt the connect.
            {
                std::lock_guard<std::mutex> lk(fd_mtx_);
                fd_ = fd;
            }
            ok = connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen);
            if (!ok) {
                last_err = errno;
                close_fd();
            }
        }
        ::freeaddrinfo(res);
        if (!ok) {
            if (!stopping_)
                LOG_WARN("slice_agent: connect to " << cfg_.host << ":" << cfg_.port
                         << " failed (" << std::strerror(last_err) << ")");
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(fd_mtx_);
            configure_socket(fd_);
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
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(cfg_.connect_timeout_ms);
            while (true) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) { errno = ETIMEDOUT; return false; }
                pollfd p{fd, POLLOUT, 0};
                rc = ::poll(&p, 1, static_cast<int>(left));
                if (rc < 0 && errno == EINTR) continue;   // signal: keep waiting
                if (rc == 0) { errno = ETIMEDOUT; return false; }
                if (rc < 0) return false;
                break;
            }
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

    // The agent never sends anything, so EOF (or a socket error) means the
    // peer has gone away. Non-blocking peek.
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
    std::condition_variable cv_;       // one CV for everything: worker wake-ups,
                                       // wait() verdicts, stop() grace
    bool stopping_{false};
    bool worker_done_{false};
    uint64_t generation_{0};           // last posted
    uint64_t sent_gen_{0};             // last generation the worker considers delivered (0 = resend)
    uint64_t sent_gen_ack_{0};         // highest generation written (for wait())
    static constexpr size_t kSentHistory = 1024;
    std::set<uint64_t> sent_gens_;     // generations actually written (exact verdicts)
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
