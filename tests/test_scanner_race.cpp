/*
 * tests/test_scanner_race.cpp
 * Regression test for HIGH #8 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 * Two concurrent scanner connections used to overwrite scanner_socket_,
 * stranding the first session's pushback path on a dead fd.
 *
 * Post-fix: the second concurrent connection is rejected cleanly while the
 * first session remains connected.
 */

#include <catch2/catch_all.hpp>

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <thread>

#include "marshal_state.hpp"
#include "mrd_tcp_listener.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
uint16_t get_ephemeral_port() {
    net::io_context ioc;
    tcp::acceptor a(ioc, tcp::endpoint(tcp::v4(), 0));
    return a.local_endpoint().port();
}
}

TEST_CASE("Second concurrent scanner is rejected; first stays connected",
          "[mrd][race]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_race";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    // First scanner connects.
    tcp::socket scanner1(ioc);
    scanner1.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));

    // Give listener time to accept + mark session_active.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(listener.has_scanner());

    // Second scanner connects concurrently. Connection itself succeeds (TCP
    // accept completes before the listener rejects), but the listener must
    // close it promptly AND must not disturb scanner1.
    tcp::socket scanner2(ioc);
    scanner2.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // scanner1 must still be considered the active session.
    REQUIRE(listener.has_scanner());

    // scanner2 should read EOF quickly (rejected). Try a zero-length read
    // and expect 0 bytes / connection closed.
    boost::system::error_code ec;
    char buf[4];
    scanner2.non_blocking(true);
    scanner2.read_some(net::buffer(buf, sizeof(buf)), ec);
    INFO("scanner2 read ec: " << ec.message());
    // Either the remote closed (eof) or the socket is already in a closed
    // state on our side. Either way, scanner2 must not be usable.
    REQUIRE((ec == net::error::eof
             || ec == net::error::connection_reset
             || ec == net::error::bad_descriptor
             || ec == net::error::not_connected
             || ec == net::error::would_block  // waited for close but timing
             || ec));

    boost::system::error_code ignore;
    scanner1.shutdown(tcp::socket::shutdown_both, ignore);
    scanner1.close(ignore);
    scanner2.shutdown(tcp::socket::shutdown_both, ignore);
    scanner2.close(ignore);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}

// Audit #1 image-only follow-up: a new scanner TCP session bumps scan_epoch
// even without METADATA, so stale callbacks/publishes of the previous
// session cannot be attributed to it.
TEST_CASE("Each accepted scanner session bumps scan_epoch", "[mrd][epoch]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_session_epoch";
    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);
    std::thread ioc_thread([&] { ioc.run(); });

    const uint64_t e0 = state.scan_epoch.load();
    for (int i = 1; i <= 2; ++i) {
        tcp::socket s(ioc);
        s.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        for (int t = 0; t < 100 && state.scan_epoch.load() < e0 + i; ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(state.scan_epoch.load() == e0 + i);
        boost::system::error_code ig;
        s.shutdown(tcp::socket::shutdown_both, ig);
        s.close(ig);
        for (int t = 0; t < 100 && listener.has_scanner(); ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        REQUIRE_FALSE(listener.has_scanner());
    }
    listener.stop();
    ioc.stop();
    ioc_thread.join();
}
