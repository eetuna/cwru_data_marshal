#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <set>
#include <mutex>
#include "marshal_state.hpp"

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
            auto data = boost::beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            // echo or route by {topic:..., payload:...}
            server.broadcast(data); // naive fan-out
        }
        void send(const std::string &s)
        {
            std::scoped_lock lk(send_mtx);
            ws.text(true);
            ws.write(boost::asio::buffer(s));
        }
    };
};