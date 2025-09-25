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
#include "realtime.hpp"
extern FrameQueue *g_queue;

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
                server.broadcast(data);
                return;
            }
            // Binary MRD1: flatten buffer sequence into a byte vector
            auto cb = buffer.data();
            std::vector<uint8_t> b;
            b.reserve(buffer.size());
            for (auto it = boost::asio::buffer_sequence_begin(cb);
                 it != boost::asio::buffer_sequence_end(cb); ++it)
            {
                auto seg = *it;
                auto p = static_cast<const uint8_t *>(seg.data());
                b.insert(b.end(), p, p + seg.size());
            }
            buffer.consume(buffer.size());
            if (b.size() < 36)
                return;
            if (!(b[0] == 'M' && b[1] == 'R' && b[2] == 'D' && b[3] == '1'))
                return;
            auto rd16 = [&](size_t o)
            { return (uint16_t)b[o] | ((uint16_t)b[o + 1] << 8); };
            auto rd64 = [&](size_t o)
            { uint64_t x=0; for(int i=0;i<8;i++) x|=((uint64_t)b[o+i])<<(8*i); return x; };
            uint16_t series_len = rd16(8);
            uint64_t frame_idx = rd64(12);
            uint64_t ts_ns = rd64(20);
            uint64_t payload_n = rd64(28);
            size_t header_len = 36 + series_len;
            if (b.size() < header_len + payload_n)
                return;
            std::string series(reinterpret_cast<const char *>(b.data() + 36), series_len);
            std::vector<uint8_t> payload(b.begin() + header_len, b.begin() + header_len + payload_n);
            if (g_queue)
                g_queue->enqueue(make_frame(series, frame_idx, ts_ns, std::move(payload)));
            // ack
            send(std::string("{\"ack\":") + std::to_string(frame_idx) + "}");
        }
        void send(const std::string &s)
        {
            std::scoped_lock lk(send_mtx);
            ws.text(true);
            ws.write(boost::asio::buffer(s));
        }
    };
};