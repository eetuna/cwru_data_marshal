/*
 * File: clients/fk_client/fk_client_main.cpp
 * Project: CWRU Data Marshal
 * Purpose: Example HTTP consumer client
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#include <iostream>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>       // for flat_buffer
#include <boost/beast/core/buffers_to_string.hpp> // if you use buffers_to_string

#include <nlohmann/json.hpp>
#include <chrono>

namespace http = boost::beast::http;
using json = nlohmann::json;

int main(int argc, char **argv)
{
    bool pretty = false;
    std::string base = "http://localhost:8080";
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--http" && i + 1 < argc)
            base = argv[++i];
        else if (std::string(argv[i]) == "--pretty")
        {
            pretty = true;
        }
    }
    // POST /v1/pose/update periodically
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver res{ioc};
    auto pos = base.find("//");
    auto hp = base.substr(pos + 2);
    auto host = hp.substr(0, hp.find(":"));
    auto port = hp.substr(host.size() + 1);
    auto results = res.resolve(host, port);
    for (int k = 0; k < 50; ++k)
    {
        boost::asio::ip::tcp::socket sock{ioc};
        boost::asio::connect(sock, results.begin(), results.end());
        http::request<http::string_body> req{http::verb::post, "/v1/pose/update", 11};
        req.set(http::field::host, host);
        req.set(http::field::content_type, "application/json");
        json j{{"p", {0.01 * k, 0.0, 0.0}}, {"R", {1, 0, 0, 0, 1, 0, 0, 0, 1}}, {"source", "fk"}};
        req.body() = j.dump();
        req.prepare_payload();
        http::write(sock, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(sock, buf, res);

        try
        {
            auto j = nlohmann::json::parse(res.body());
            std::cout << "[fk_client] status=" << res.result_int() << " body:\n";
            if (pretty)
            {
                std::cout << j.dump(2) << std::endl; // pretty JSON
            }
            else
            {
                std::cout << j.dump() << std::endl; // compact JSON
            }
        }
        catch (const std::exception &e)
        {
            std::cout << "[fk_client] status=" << res.result_int()
                      << " raw body=" << res.body()
                      << " (failed to parse JSON: " << e.what() << ")" << std::endl;
        }

        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}