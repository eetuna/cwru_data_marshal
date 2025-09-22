/*
 * File: clients/fk_client/fk_client_main.cpp
 * Project: CWRU Data Marshal
 * Purpose: Example HTTP consumer client
 * Notes:
 *  - Very small changes: 3x3 R matrix, include <thread>, safe socket shutdown
 * Last updated: 2025-09-21 (minimal fix)
 */

#include <iostream>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread> // << added

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
        else if (a == "--pretty")
            pretty = true;
    }

    // POST /v1/pose/update periodically
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver res{ioc};

    // parse http://host:port (no path support needed here)
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

        // Minimal valid payload the server wants: p (vec3), R (3x3 matrix)
        // Keep it dead simple; identity rotation is fine.
        json j{
            {"p", {0.01 * k, 0.0, 0.0}},
            // Flattened 3x3 identity (row-major):
            {"R", {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}},
            {"source", "fk"}};

        req.body() = j.dump();
        req.prepare_payload();

        http::write(sock, req);

        boost::beast::flat_buffer buf;
        http::response<http::string_body> res_msg;
        http::read(sock, buf, res_msg);

        try
        {
            auto parsed = nlohmann::json::parse(res_msg.body());
            std::cout << "[fk_client] status=" << res_msg.result_int() << " body:\n";
            if (pretty)
                std::cout << parsed.dump(2) << std::endl;
            else
                std::cout << parsed.dump() << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cout << "[fk_client] status=" << res_msg.result_int()
                      << " raw body=" << res_msg.body()
                      << " (failed to parse JSON: " << e.what() << ")\n";
        }

        // Avoid throwing if peer already closed
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
