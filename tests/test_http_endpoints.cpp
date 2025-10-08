/*
 * File: tests/test_http_endpoints.cpp
 * Project: CWRU Data Marshal
 * Purpose: HTTP routing and handlers
 * Notes:
 *  - See docs/PURPOSE.md and docs/ARCHITECTURE.md
 *  - Atomic file writes via include/atomic_write.hpp
 *  - /health returns constant JSON; no shared state
 *  - WebSocket ping/pong keepalive recommended
 * Last updated: 2025-09-15
 */

#include <catch2/catch_all.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/test/stream.hpp>
#include <sstream>

#include "marshal_http.hpp"

using boost::beast::http::error::body_limit;

namespace
{
std::string make_http_post(const std::string &target, const std::string &body)
{
    std::ostringstream oss;
    oss << "POST " << target << " HTTP/1.1\r\n"
        << "Host: example\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return oss.str();
}
} // namespace

TEST_CASE("HTTP parser limit is raised for large payloads")
{
    const std::size_t payload_bytes = 2 * 1024 * 1024; // 2 MiB > Beast's 1 MiB default
    std::string body(payload_bytes, 'x');
    const std::string raw = make_http_post("/v1/pose/update", body);

    SECTION("default limit rejects the payload")
    {
        boost::asio::io_context ioc;
        boost::beast::test::stream<> stream{ioc};
        stream.append(raw);

        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;

        boost::system::error_code ec;
        boost::beast::http::read(stream, buffer, req, ec);

        REQUIRE(ec == body_limit);
    }

    SECTION("raised limit accepts the payload")
    {
        boost::asio::io_context ioc;
        boost::beast::test::stream<> stream{ioc};
        stream.append(raw);

        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;
        req.version(11);
        req.body_limit(kMaxHttpBodyBytes);

        boost::system::error_code ec;
        boost::beast::http::read(stream, buffer, req, ec);

        REQUIRE_FALSE(ec);
        REQUIRE(req.body().size() == payload_bytes);
    }
}
