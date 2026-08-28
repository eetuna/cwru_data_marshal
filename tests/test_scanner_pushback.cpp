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

TEST_CASE("push_message_to_scanner unblocks when stop() closes socket",
          "[mrd][pushback][shutdown]") {
    // 2026-04-19 round-7: pushback is now inline ::send. If the
    // scanner socket's recv buffer fills, send() blocks -- this is
    // the standard TCP flow-control mechanism that python-ismrmrd-server
    // and every other MRD peer relies on. The shutdown-safety property
    // that matters is: a blocked pushback call must unblock when the
    // listener's stop() closes the scanner fd. Test:
    //   1. Connect a stuck-scanner client that never reads.
    //   2. Launch a thread that hammers push_message_to_scanner; once
    //      the kernel buffer fills, that thread blocks in ::send.
    //   3. Call listener.stop() from the main thread. It closes the
    //      scanner socket, which returns EPIPE on the blocked send.
    //   4. The hammer thread returns and joins within a timeout.
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_pushback_stuck";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    tcp::socket stuck_scanner(ioc);
    stuck_scanner.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::string body(256 * 1024, 'Y'); // 256 KB each
    std::atomic<bool> hammer_done{false};
    std::atomic<int> hammer_sent{0};
    std::thread hammer([&] {
        for (int i = 0; i < 1000; ++i) {
            listener.push_message_to_scanner(0x1022, body.data(), body.size());
            hammer_sent.fetch_add(1);
            // If the socket gets closed by stop(), write_exact_fd
            // returns immediately and we loop fast -- cap iterations
            // to avoid a tight spin after unblock.
            if (hammer_done.load()) break;
        }
        hammer_done.store(true);
    });

    // Give the hammer a moment to fill the scanner's recv buffer and
    // block in ::send.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto stop_start = std::chrono::steady_clock::now();
    listener.stop();  // must unblock the hammer thread
    hammer.join();
    auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start).count();

    INFO("stop() + hammer.join() took " << stop_ms << " ms, hammer_sent="
         << hammer_sent.load());
    // Should be well under a second; stop() closes the fd and the
    // blocked send returns EPIPE immediately.
    REQUIRE(stop_ms < 2000);

    boost::system::error_code ignore;
    stuck_scanner.shutdown(tcp::socket::shutdown_both, ignore);
    stuck_scanner.close(ignore);

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

    INFO("received " << received.load() << " / " << kCount);
    REQUIRE(received.load() == kCount);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}

TEST_CASE("push_message_to_scanner serializes concurrent senders",
          "[mrd][pushback][ordering]") {
    // Regression for codex round-8 finding #1: without scanner_send_mtx_,
    // two threads calling push_message_to_scanner could interleave the
    // tag+body pair of distinct MRD frames on the wire. The reader
    // would then parse a tag from frame A followed by a body from
    // frame B, corrupting both.
    //
    // Fires N frames from two producer threads, each with a distinct
    // tag and a distinctive body pattern (all 'A's vs all 'B's).
    // Asserts that every received frame has a body matching its tag's
    // pattern end-to-end (i.e. no interleave).
    MarshalState state;
    state.dump_dir = "/tmp/test_scanner_pushback_ordering";

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);

    std::thread ioc_thread([&] { ioc.run(); });

    tcp::socket scanner(ioc);
    scanner.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    constexpr int kPerThread = 500;
    constexpr uint16_t kTagA = 0x1022;
    constexpr uint16_t kTagB = 0x1026;
    const std::string bodyA(512, 'A');
    const std::string bodyB(512, 'B');

    std::atomic<int> received_a{0};
    std::atomic<int> received_b{0};
    std::atomic<bool> saw_corruption{false};
    std::atomic<bool> reader_stop{false};
    std::thread reader([&] {
        boost::system::error_code ec;
        while (!reader_stop.load()) {
            uint16_t got_tag = 0;
            auto n = boost::asio::read(scanner,
                boost::asio::buffer(&got_tag, sizeof(got_tag)), ec);
            if (ec || n != sizeof(got_tag)) return;
            std::vector<char> buf(512);
            auto nb = boost::asio::read(scanner,
                boost::asio::buffer(buf), ec);
            if (ec || nb != buf.size()) return;
            char expected = 0;
            if (got_tag == kTagA) { received_a.fetch_add(1); expected = 'A'; }
            else if (got_tag == kTagB) { received_b.fetch_add(1); expected = 'B'; }
            else { saw_corruption.store(true); return; }
            for (char c : buf) {
                if (c != expected) { saw_corruption.store(true); return; }
            }
        }
    });

    std::thread producer_a([&] {
        for (int i = 0; i < kPerThread; ++i) {
            listener.push_message_to_scanner(kTagA, bodyA.data(), bodyA.size());
        }
    });
    std::thread producer_b([&] {
        for (int i = 0; i < kPerThread; ++i) {
            listener.push_message_to_scanner(kTagB, bodyB.data(), bodyB.size());
        }
    });
    producer_a.join();
    producer_b.join();

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(10);
    while ((received_a.load() + received_b.load()) < 2 * kPerThread &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    reader_stop.store(true);
    boost::system::error_code ignore;
    scanner.shutdown(tcp::socket::shutdown_both, ignore);
    scanner.close(ignore);
    if (reader.joinable()) reader.join();

    INFO("received A=" << received_a.load() << " B=" << received_b.load()
         << " corruption=" << saw_corruption.load());
    REQUIRE_FALSE(saw_corruption.load());
    REQUIRE(received_a.load() == kPerThread);
    REQUIRE(received_b.load() == kPerThread);

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}

// ---------------------------------------------------------------------------
// Audit 2026-08-28 #5: a recon that accepts the connection but never reads
// must not hold a sender hostage. Two bounds:
//   - cancel() (shutdown path) wakes a thread blocked in send() immediately;
//   - TCP_USER_TIMEOUT aborts the wedged connection on its own.
// The fake recon shrinks its receive buffer and never calls recv, so the
// forwarder's blocking send() wedges after a few hundred KB.
// ---------------------------------------------------------------------------
namespace {
struct HungRecon {
    net::io_context ioc;
    tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), 0)};
    std::unique_ptr<tcp::socket> peer;   // accepted, never read
    std::thread th;
    uint16_t port() const { return acceptor.local_endpoint().port(); }
    void start() {
        th = std::thread([this] {
            auto s = std::make_unique<tcp::socket>(ioc);
            boost::system::error_code ec;
            acceptor.accept(*s, ec);
            if (!ec) {
                net::socket_base::receive_buffer_size rb(4096);
                s->set_option(rb, ec);
                peer = std::move(s);
            }
        });
    }
    ~HungRecon() {
        boost::system::error_code ignore;
        acceptor.close(ignore);
        if (th.joinable()) th.join();
        if (peer) peer->close(ignore);
    }
};

// Push frames until send() blocks; returns once send fails (socket aborted).
std::thread wedge_sender(mrd::ReconForwarder& fwd, std::atomic<bool>& returned) {
    return std::thread([&] {
        std::vector<uint8_t> chunk(64 * 1024, 0xAB);
        while (fwd.is_connected()) {
            fwd.post_frame(mrd::MRD_MESSAGE_ISMRMRD_ACQUISITION, chunk);
        }
        returned.store(true);
    });
}
} // namespace

TEST_CASE("cancel() wakes a sender blocked on a recon that stopped reading",
          "[recon_forwarder][hung_recon]") {
    HungRecon recon;
    recon.start();
    std::atomic<int> failures{0};
    mrd::ReconForwarder fwd("127.0.0.1", recon.port(),
                            [](uint64_t, uint16_t, const void*, size_t) {},
                            [&](uint64_t) { failures.fetch_add(1); },
                            2000 /*connect ms*/, 60000 /*user timeout: not the bound here*/);
    // Small send buffer so the wedge happens quickly.
    REQUIRE(fwd.begin_session());

    std::atomic<bool> returned{false};
    auto sender = wedge_sender(fwd, returned);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(returned.load());   // sender is wedged in send()

    const auto t0 = std::chrono::steady_clock::now();
    fwd.cancel();
    sender.join();
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(returned.load());
    CHECK(took < std::chrono::seconds(2));
    fwd.stop();
}

TEST_CASE("TCP_USER_TIMEOUT aborts a recon connection that stopped reading",
          "[recon_forwarder][hung_recon]") {
    HungRecon recon;
    recon.start();
    std::atomic<int> failures{0};
    mrd::ReconForwarder fwd("127.0.0.1", recon.port(),
                            [](uint64_t, uint16_t, const void*, size_t) {},
                            [&](uint64_t) { failures.fetch_add(1); },
                            2000, 1000 /*user timeout ms*/);
    REQUIRE(fwd.begin_session());

    std::atomic<bool> returned{false};
    auto sender = wedge_sender(fwd, returned);
    const auto t0 = std::chrono::steady_clock::now();
    sender.join();   // returns only when the kernel aborts the connection
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(returned.load());
    CHECK_FALSE(fwd.is_connected());
    CHECK(failures.load() == 1);
    // 1 s user timeout + probe scheduling slack; the untreated case never returns.
    CHECK(took < std::chrono::seconds(15));
    fwd.stop();
}

// Audit #1 follow-up: the epoch a callback reports is the one its CONNECTION
// was opened with, not whatever the listener has stamped since.
namespace {
struct EchoRecon {   // accepts, then sends one IMAGE-tagged frame with an empty header? No: a TEXT frame.
    net::io_context ioc;
    tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), 0)};
    std::thread th;
    uint16_t port() const { return acceptor.local_endpoint().port(); }
    void start() {
        th = std::thread([this] {
            for (int i = 0; i < 2; ++i) {
                tcp::socket s(ioc);
                boost::system::error_code ec;
                acceptor.accept(s, ec);
                if (ec) return;
                // [tag=TEXT][uint32 len=2]["x\0"]
                uint16_t tag = mrd::MRD_MESSAGE_TEXT; uint32_t len = 2;
                std::vector<uint8_t> f(2 + 4 + 2, 0);
                std::memcpy(f.data(), &tag, 2); std::memcpy(f.data() + 2, &len, 4); f[6] = 'x';
                net::write(s, net::buffer(f), ec);
                // keep open until the client closes
                uint8_t b; s.read_some(net::buffer(&b, 1), ec);
            }
        });
    }
    ~EchoRecon() { boost::system::error_code ig; acceptor.close(ig); if (th.joinable()) th.join(); }
};
} // namespace

TEST_CASE("recon callbacks carry the epoch of the connection that produced them",
          "[recon_forwarder][epoch]") {
    EchoRecon recon;
    recon.start();
    std::mutex m; std::vector<uint64_t> seen;
    mrd::ReconForwarder fwd("127.0.0.1", recon.port(),
        [&](uint64_t e, uint16_t, const void*, size_t) { std::lock_guard<std::mutex> lk(m); seen.push_back(e); },
        [&](uint64_t) {}, 2000);
    REQUIRE(fwd.begin_session(7));
    for (int i = 0; i < 100 && [&]{ std::lock_guard<std::mutex> lk(m); return seen.empty(); }(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(fwd.begin_session(8));   // joins the old reader first
    for (int i = 0; i < 100 && [&]{ std::lock_guard<std::mutex> lk(m); return seen.size() < 2; }(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    fwd.stop();
    std::lock_guard<std::mutex> lk(m);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 7);
    CHECK(seen[1] == 8);
    // Epoch-checked CLOSE for a superseded epoch is a no-op (no failure, no send).
}
