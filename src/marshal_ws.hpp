/*
 * File: src/marshal_ws.hpp
 * Project: CWRU Data Marshal
 * Purpose: Internal support module
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 *  - PERFORMANCE FIX: Uses async writes with queue to prevent blocking
 * Last updated: 2025-09-21
 */

#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <set>
#include <mutex>
#include <queue>
#include <atomic>
#include "marshal_state.hpp"
#include <boost/asio/buffer.hpp>
#include <nlohmann/json.hpp>
#include "mrd_io.hpp"

namespace websocket = boost::beast::websocket;

// Helper struct for WS handler results
struct WsResult
{
    std::string response;      // Send to caller only
    std::string broadcast;     // Send to all (or topic)
    std::string topic;         // New topic if updated
    bool is_subscription = false;
};

inline WsResult handle_ws_message(MarshalState &state, const std::string &text_data, const std::vector<uint8_t> &binary_data, const std::string &current_topic)
{
    WsResult res;
    res.topic = current_topic; // default: no change

    // 1. Text Message Handling
    if (!text_data.empty())
    {
        // Try to parse control JSON: {"subscribe":"<topic>"} or {"unsubscribe":"<topic>"}
        try
        {
            nlohmann::json j = nlohmann::json::parse(text_data, nullptr, false);
            if (!j.is_discarded())
            {
                if (j.contains("subscribe") && j["subscribe"].is_string())
                {
                    res.topic = j["subscribe"].get<std::string>();
                    res.is_subscription = true;
                    res.response = "{\"ok\":true,\"subscribed\":\"" + res.topic + "\"}";
                    return res;
                }
                if (j.contains("unsubscribe") && j["unsubscribe"].is_string())
                {
                    // Revert to default system topic
                    res.topic = "_system_"; 
                    res.is_subscription = true;
                    res.response = "{\"ok\":true,\"unsubscribed\":true}";
                    return res;
                }
            }
        }
        catch (...)
        {
            // ignore
        }

        // Not a control message -> Broadcast it
        res.broadcast = text_data;
        return res;
    }

    // 2. Binary Message Handling
    if (binary_data.empty())
    {
        res.response = "{\"error\":\"empty payload\"}";
        return res;
    }

    try
    {
        auto entry = mrd::ingest_payload(state, binary_data.data(), binary_data.size(), "ws");
        res.response = entry.dump();
    }
    catch (const std::exception &e)
    {
        nlohmann::json err = {{"error", "ws ingest failed"}, {"what", e.what()}};
        res.response = err.dump();
    }

    return res;
}

class WsServer
{
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket socket_;
    MarshalState &state_;

public:
    WsServer(boost::asio::io_context &ioc, boost::asio::ip::tcp::endpoint ep, MarshalState &s)
        : acceptor_(ioc), socket_(ioc), state_(s)
    {
        // Set the emit callback so HTTP handlers can push to all WS clients
        state_.ws_emit = [this](const std::string &msg)
        {
            this->broadcast(msg);
        };
        state_.ws_emit_topic = [this](const std::string &msg, const std::string &topic)
        {
            this->broadcast_to(msg, topic);
        };

        boost::system::error_code ec;
        acceptor_.open(ep.protocol(), ec);
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        acceptor_.bind(ep, ec);
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        do_accept();
    }
    void broadcast(const std::string &msg)
    {
        // Copy client list under lock, then release lock before sending
        std::vector<void *> clients_copy;
        {
            std::scoped_lock lk(state_.ws_mtx);
            clients_copy.assign(state_.ws_clients.begin(), state_.ws_clients.end());
        }

        for (auto h : clients_copy)
        {
            try {
                auto *s = static_cast<Session *>(h);
                s->send(msg);
            } catch (const std::exception& e) {
                std::cerr << "WS individual broadcast failed: " << e.what() << "\n";
            }
        }
    }
    void broadcast_to(const std::string &msg, const std::string &topic)
    {
        // Copy client list under lock, then release lock before sending
        std::vector<void *> clients_copy;
        {
            std::scoped_lock lk(state_.ws_mtx);
            clients_copy.assign(state_.ws_clients.begin(), state_.ws_clients.end());
        }

        for (auto h : clients_copy)
        {
            try {
                auto *s = static_cast<Session *>(h);
                if (topic.empty() || s->topic == topic)
                    s->send(msg);
            } catch (const std::exception& e) {
                std::cerr << "WS topic broadcast failed: " << e.what() << "\n";
            }
        }
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(socket_, [this](auto ec)
                               {
if(!ec) std::make_shared<Session>(std::move(socket_), state_, *this)->run();
do_accept(); });
    }
    struct Session : std::enable_shared_from_this<Session>
    {
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws;
        boost::beast::flat_buffer buffer;
        MarshalState &state;
        WsServer &server;
        std::string topic = "_system_"; // Default topic, receives only global broadcasts

        // Async write queue to prevent blocking
        std::mutex queue_mtx_;
        std::queue<std::string> write_queue_;
        std::atomic<bool> write_in_progress_{false};
        std::atomic<bool> closed_{false};

        // Maximum queue size to prevent memory bloat from slow clients
        static constexpr size_t kMaxQueueSize = 1000;

        Session(boost::asio::ip::tcp::socket &&s, MarshalState &st, WsServer &sv)
            : ws(std::move(s)), state(st), server(sv) {}

        void run()
        {
            ws.set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
            ws.accept();
            {
                std::scoped_lock lk(state.ws_mtx);
                state.ws_clients.insert(this);
            }
            do_read();
        }

        ~Session()
        {
            closed_ = true;
            // Remove self from clients list
            // Since broadcast() now copies the list and releases the lock before sending,
            // the destructor taking the lock is safe (broadcast won't call methods on us)
            std::scoped_lock lk(state.ws_mtx);
            state.ws_clients.erase(this);
        }

        void do_read()
        {
            auto self = shared_from_this();
            ws.async_read(buffer, [self](auto ec, auto)
                          { if(!ec){ self->on_msg(); self->do_read(); } });
        }

        void on_msg()
        {
            std::string text_data;
            std::vector<uint8_t> binary_data;

            if (ws.got_text())
            {
                text_data = boost::beast::buffers_to_string(buffer.data());
            }
            else
            {
                auto cb = buffer.data();
                binary_data.reserve(buffer.size());
                for (auto it = boost::asio::buffer_sequence_begin(cb);
                     it != boost::asio::buffer_sequence_end(cb); ++it)
                {
                    auto seg = *it;
                    auto p = static_cast<const uint8_t *>(seg.data());
                    binary_data.insert(binary_data.end(), p, p + seg.size());
                }
            }
            buffer.consume(buffer.size());

            WsResult res = handle_ws_message(state, text_data, binary_data, topic);

            if (res.is_subscription)
            {
                topic = res.topic;
            }

            if (!res.response.empty())
            {
                send(res.response);
            }

            if (!res.broadcast.empty())
            {
                server.broadcast(res.broadcast);
            }
        }

        // Non-blocking send - queues message for async delivery
        void send(const std::string &s)
        {
            if (closed_)
                return;

            std::string line = s + "\n";

            {
                std::lock_guard<std::mutex> lk(queue_mtx_);

                // Drop messages if queue is too large (slow client protection)
                if (write_queue_.size() >= kMaxQueueSize)
                {
                    // Log dropped message but don't block
                    std::cerr << "WS: dropping message, queue full for slow client\n";
                    return;
                }

                write_queue_.push(std::move(line));
            }

            // Start async write chain if not already in progress
            maybe_start_write();
        }

    private:
        void maybe_start_write()
        {
            // Only one write can be in progress at a time
            bool expected = false;
            if (!write_in_progress_.compare_exchange_strong(expected, true))
                return;

            do_write();
        }

        void do_write()
        {
            if (closed_)
            {
                write_in_progress_ = false;
                return;
            }

            std::string msg;
            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                if (write_queue_.empty())
                {
                    write_in_progress_ = false;
                    return;
                }
                msg = std::move(write_queue_.front());
                write_queue_.pop();
            }

            auto self = shared_from_this();
            ws.text(true);
            ws.async_write(boost::asio::buffer(msg),
                [self, msg_copy = std::move(msg)](boost::beast::error_code ec, std::size_t) {
                    if (ec)
                    {
                        // Connection error, stop writing
                        self->closed_ = true;
                        self->write_in_progress_ = false;
                        return;
                    }
                    // Continue writing if more messages in queue
                    self->do_write();
                });
        }
    };
};