/*
 * tests/test_scanner_pushback.cpp
 * Regression test for HIGH #4 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 * push_message_to_scanner used to do a blocking ::send inline, which could
 * block the recon reader thread when the scanner socket was stuck or absent.
 *
 * Post-fix: push_message_to_scanner enqueues onto an async writer thread and
 * returns immediately even when no scanner is connected.
 */

#include <catch2/catch_all.hpp>

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <thread>

#include "marshal_state.hpp"
#include "mrd_tcp_listener.hpp"
#include "recon_forwarder.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Pick an ephemeral port by binding to port 0, then releasing.
uint16_t get_ephemeral_port() {
    net::io_context ioc;
    tcp::acceptor a(ioc, tcp::endpoint(tcp::v4(), 0));
    return a.local_endpoint().port();
}

} // namespace

TEST_CASE("push_message_to_scanner returns promptly when no scanner connected",
          "[mrd][pushback]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_pushback";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    // No scanner connects. Pushing should be near-instant because the writer
    // thread simply drops the job (no scanner fd).
    const std::string body(128 * 1024, 'X'); // 128 KB payload
    const int iterations = 100;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        listener.push_message_to_scanner(0x1022, body.data(), body.size());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Pre-fix, this would hang or fail-fast per iteration; post-fix it's a
    // pure memcpy + notify per call. Allow generous 500ms budget for 100
    // iterations of 128KB enqueues (bounded queue may drop internally).
    INFO("Elapsed: " << elapsed_ms << " ms for " << iterations << " pushes");
    REQUIRE(elapsed_ms < 500);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}

TEST_CASE("push_message_to_scanner survives stuck scanner",
          "[mrd][pushback]") {
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_pushback_stuck";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    // Connect a "scanner" client that never reads. Its recv buffer will fill
    // up under large sends. The pre-fix blocking ::send would hang the caller
    // once the buffer fills.
    tcp::socket stuck_scanner(ioc);
    stuck_scanner.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));

    // Give the listener a moment to accept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Fire a large number of big messages. With the async writer, the
    // writer thread may block on ::send once the scanner's recv buffer
    // is full; push_message_to_scanner itself must remain prompt.
    // 2026-04-19: pushback is now unbounded and lossless (contract
    // says recon return messages reach the scanner unconditionally).
    // Memory grows while the scanner is stuck; growth is surfaced as
    // a high-watermark warning but no records are dropped.
    const std::string body(256 * 1024, 'Y'); // 256 KB each
    const int iterations = 50;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        listener.push_message_to_scanner(0x1022, body.data(), body.size());
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    INFO("Elapsed: " << elapsed_ms << " ms for " << iterations
         << " pushes to stuck scanner");
    // Caller path must be fast even with a stuck scanner.
    REQUIRE(elapsed_ms < 1000);

    boost::system::error_code ignore;
    stuck_scanner.shutdown(tcp::socket::shutdown_both, ignore);
    stuck_scanner.close(ignore);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}

TEST_CASE("push_message_to_scanner is lossless past the old 1024 cap",
          "[mrd][pushback][lossless]") {
    // Regression for codex round-5 finding: the scanner writer queue
    // used to drop oldest once it reached 1024 entries. That violated
    // the contract clause that recon return messages are pushed back
    // to the scanner unconditionally.
    //
    // Post-fix: queue is unbounded; high-watermark warning at 10k
    // but no drop. This test pushes well past the old 1024 cap
    // through a live scanner socket and verifies the exact count
    // arrives on the other side.
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_pushback_lossless";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    tcp::socket scanner(ioc);
    scanner.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    constexpr int kCount = 2000;
    constexpr uint16_t kTag = 0x1022; // IMAGE
    const std::string body(128, 'Z');

    std::atomic<int> received{0};
    std::atomic<bool> reader_stop{false};
    std::thread reader([&] {
        boost::system::error_code ec;
        while (!reader_stop.load()) {
            uint16_t got_tag = 0;
            auto n = boost::asio::read(scanner,
                boost::asio::buffer(&got_tag, sizeof(got_tag)), ec);
            if (ec || n != sizeof(got_tag)) return;
            std::vector<char> buf(body.size());
            auto nb = boost::asio::read(scanner,
                boost::asio::buffer(buf), ec);
            if (ec || nb != buf.size()) return;
            received.fetch_add(1);
        }
    });

    for (int i = 0; i < kCount; ++i) {
        listener.push_message_to_scanner(kTag, body.data(), body.size());
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(10);
    while (received.load() < kCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    reader_stop.store(true);
    boost::system::error_code ignore;
    scanner.shutdown(tcp::socket::shutdown_both, ignore);
    scanner.close(ignore);
    if (reader.joinable()) reader.join();

    INFO("received " << received.load() << " / " << kCount
         << " (peak queue " << listener.writer_queue_peak() << ")");
    REQUIRE(received.load() == kCount);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}
