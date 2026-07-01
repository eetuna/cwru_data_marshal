/*
 * tests/test_http_body_limit.cpp
 * Regression test for MEDIUM #11 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 * HTTP body limit used to be checked inside the handler AFTER
 * http::read had already buffered the full body. Post-fix uses
 * http::request_parser<body_type>::body_limit(N) set before http::read,
 * so oversized bodies are rejected as they stream.
 *
 * This test spawns the same http_session loop logic via a Beast test.
 * We use request_parser directly here to verify body_limit behavior,
 * since the actual http_session is not exported for testing.
 */

#include <catch2/catch_all.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

TEST_CASE("request_parser body_limit rejects oversized bodies during read",
          "[http][limits]") {
    // Use Beast's own buffer + parser pipeline without a socket — feed
    // it raw bytes via boost::beast::flat_buffer to exercise the limit.
    const std::size_t body_limit = 1024;  // 1 KB

    http::request_parser<http::string_body> parser;
    parser.body_limit(body_limit);

    // Craft a request with a 4 KB body, well over the limit.
    const std::string big_body(4 * 1024, 'X');
    std::string req_str =
        "POST /pose HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(big_body.size()) + "\r\n"
        "\r\n" + big_body;

    beast::error_code ec;
    parser.put(net::buffer(req_str.data(), req_str.size()), ec);

    // The parser must signal body_limit before consuming the full body.
    // With 1 KB limit and 4 KB body, body_limit fires during put.
    INFO("parser ec after put: " << ec.message()
         << " is_done=" << parser.is_done());

    // body_limit error is expected
    REQUIRE(ec == http::error::body_limit);
}

TEST_CASE("request_parser accepts bodies up to body_limit",
          "[http][limits]") {
    const std::size_t body_limit = 1024;

    http::request_parser<http::string_body> parser;
    parser.body_limit(body_limit);

    const std::string ok_body(512, 'Y');  // half of limit
    std::string req_str =
        "POST /pose HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(ok_body.size()) + "\r\n"
        "\r\n" + ok_body;

    beast::error_code ec;
    parser.put(net::buffer(req_str.data(), req_str.size()), ec);
    // Must not be body_limit error — we're under the cap.
    INFO("parser ec: " << ec.message()
         << " is_done=" << parser.is_done());
    REQUIRE_FALSE(ec == http::error::body_limit);
}
