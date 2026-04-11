/*
 * Tests for HTTP endpoint body-size limits.
 * Scanner/recon data uses MRD TCP; HTTP is query/control only.
 */

#include <catch2/catch_all.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <sstream>
#include <string>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "mrd_sink.hpp"

namespace http = boost::beast::http;

TEST_CASE("Body larger than max_body_bytes is rejected", "[http][limits]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_http_limits";
    state.max_body_bytes = 1024; // 1 KB limit for test

    // Create a body larger than the limit
    std::string big_body(2048, 'X');

    http::request<http::string_body> req{http::verb::post, "/pose", 11};
    req.body() = big_body;
    req.prepare_payload();

    http::response<http::string_body> res;
    handle_http_request(std::move(req), state, [&](auto&& r) { res = std::move(r); });

    REQUIRE(res.result() == http::status::payload_too_large);
}

TEST_CASE("Body within limit is accepted", "[http][limits]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_http_limits";
    state.max_body_bytes = 128 * 1024 * 1024;

    // /health has no body size concern
    http::request<http::string_body> req{http::verb::get, "/health", 11};
    req.prepare_payload();

    http::response<http::string_body> res;
    handle_http_request(std::move(req), state, [&](auto&& r) { res = std::move(r); });

    REQUIRE(res.result() == http::status::ok);
}
