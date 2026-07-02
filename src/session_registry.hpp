/*
 * File: src/session_registry.hpp
 * Purpose: Track active HTTP / MRD session threads so shutdown can close their
 *          sockets and join them before shared state is destroyed.
 *
 * Context: fixes CRITICAL #1-3 from MRI_MARSHAL_BUGS_FINAL_2026-04-18.md —
 *          detached session threads capturing MarshalState by reference could
 *          outlive main() and dereference destroyed state.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mrd {

// A single tracked session. The cancel callback should be safe to call
// concurrently with the session thread and should unblock any blocking I/O
// the session is doing (e.g. by shutdown()/close() on its socket).
struct TrackedSession {
    std::thread thread;
    std::function<void()> cancel;
};

// Registry of active sessions. Call register_session() when a new session
// starts. Call unregister_session() when it exits normally. Call shutdown()
// once during graceful shutdown to cancel and join everything.
class SessionRegistry {
public:
    SessionRegistry() = default;
    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // Register a new session. Returns an opaque id for unregister. If shutdown
    // is already in progress, the session missed shutdown_and_join's snapshot:
    // the registry cancels and joins it HERE and returns 0. Callers must not
    // touch the (moved-from) session afterwards. Destroying a joinable
    // std::thread would std::terminate, so the join is not optional; it is
    // safe because the caller is the accept handler, never the session thread
    // itself, and cancel() unblocks the session's I/O so it exits promptly.
    uint64_t register_session(TrackedSession session) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!shutting_down_) {
                uint64_t id = ++next_id_;
                sessions_.emplace_back(id, std::move(session));
                return id;
            }
        }
        try {
            if (session.cancel) session.cancel();
        } catch (...) {
            // Cancel callbacks must not throw, but be defensive.
        }
        if (session.thread.joinable()) session.thread.join();
        return 0;
    }

    // Unregister after session exits normally. Must be called from the session
    // thread itself (or after it has been joined). If called from within the
    // session thread, the thread handle is detached here so the destructor of
    // the TrackedSession does not terminate the program.
    void unregister_session(uint64_t id) {
        if (id == 0) return;
        std::thread to_detach;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = std::find_if(sessions_.begin(), sessions_.end(),
                                   [id](const auto& p) { return p.first == id; });
            if (it == sessions_.end()) return;
            to_detach = std::move(it->second.thread);
            sessions_.erase(it);
        }
        if (to_detach.joinable()) to_detach.detach();
    }

    // Called once on shutdown. Marks shutting-down (so new register_session
    // calls are rejected), invokes every session's cancel callback, then joins
    // every thread. Safe to call from outside any session thread.
    void shutdown_and_join() {
        std::vector<std::pair<uint64_t, TrackedSession>> to_join;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (shutting_down_) return;
            shutting_down_ = true;
            to_join = std::move(sessions_);
            sessions_.clear();
        }
        // Cancel first so blocked I/O unblocks, then join.
        for (auto& [id, sess] : to_join) {
            try {
                if (sess.cancel) sess.cancel();
            } catch (...) {
                // Cancel callbacks must not throw, but be defensive.
            }
        }
        for (auto& [id, sess] : to_join) {
            if (sess.thread.joinable()) sess.thread.join();
        }
    }

    // True if shutdown_and_join() has been called.
    bool shutting_down() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return shutting_down_;
    }

    ~SessionRegistry() {
        // Defensive: if the owner forgot to call shutdown_and_join, at least
        // avoid terminating on joinable threads. This is a bug if reached;
        // we don't have a logger handle here so just detach.
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [id, sess] : sessions_) {
            if (sess.thread.joinable()) sess.thread.detach();
        }
    }

private:
    mutable std::mutex mtx_;
    std::vector<std::pair<uint64_t, TrackedSession>> sessions_;
    uint64_t next_id_{0};
    bool shutting_down_{false};
};

} // namespace mrd
