/*
 * File: src/marshal_ws.hpp
 * Project: CWRU Data Marshal
 * Purpose: Internal support module
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <set>
#include <mutex>
#include "marshal_state.hpp"
#include <boost/asio/buffer.hpp>
#include <nlohmann/json.hpp>
#include "mrd_io.hpp"

namespace websocket = boost::beast::websocket;

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
        std::scoped_lock lk(state_.ws_mtx);
        for (auto h : state_.ws_clients)
        {
            auto *s = static_cast<Session *>(h);
            s->send(msg);
        }
    }
    void broadcast_to(const std::string &msg, const std::string &topic)
    {
        std::scoped_lock lk(state_.ws_mtx);
        for (auto h : state_.ws_clients)
        {
            auto *s = static_cast<Session *>(h);
            // deliver if the session is subscribed to this topic or to ALL (empty)
            if (topic.empty() || s->topic.empty() || s->topic == topic)
                s->send(msg);
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
        std::mutex send_mtx;
        std::string topic; // empty = receive ALL broadcasts
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
            if (ws.got_text())
            {
                auto data = boost::beast::buffers_to_string(buffer.data());
                buffer.consume(buffer.size());

                // Try to parse control JSON: {"subscribe":"<topic>"}
                try
                {
                    nlohmann::json j = nlohmann::json::parse(data);
                    if (j.contains("subscribe") && j["subscribe"].is_string())
                    {
                        topic = j["subscribe"].get<std::string>();
                        // Ack only to this client (do not broadcast the control)
                        send(std::string("{\"ok\":true,\"subscribed\":\"") + topic + "\"}");
                        return;
                    }
                }
                catch (...)
                {
                    // not JSON/control; fall through to broadcast as-is
                }

                // Normal text: echo/broadcast to all sessions (including sender), as before
                server.broadcast(data);
                return;
            }

            auto cb = buffer.data();
            std::vector<uint8_t> payload;
            payload.reserve(buffer.size());
            for (auto it = boost::asio::buffer_sequence_begin(cb);
                 it != boost::asio::buffer_sequence_end(cb); ++it)
            {
                auto seg = *it;
                auto p = static_cast<const uint8_t *>(seg.data());
                payload.insert(payload.end(), p, p + seg.size());
            }
            buffer.consume(buffer.size());
            if (payload.empty())
            {
                send("{\"error\":\"empty payload\"}");
                return;
            }
            try
            {
                auto entry = mrd::ingest_payload(state, payload.data(), payload.size(), "ws");
                send(entry.dump());
            }
            catch (const std::exception &e)
            {
                nlohmann::json err = {{"error", "ws ingest failed"}, {"what", e.what()}};
                send(err.dump());
            }
        }
        void send(const std::string &s)
        {
            std::scoped_lock lk(send_mtx);
            ws.text(true);
            // Send one text frame that ends with a newline so CLIs and pipes flush immediately
            std::string line = s;
            line.push_back('\n');
            ws.write(boost::asio::buffer(line));
        }
        };
};