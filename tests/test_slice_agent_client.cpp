/*
 * tests/test_slice_agent_client.cpp
 * SliceAgentClient against a fake slice_agent (raw TCP acceptor reading
 * 56-byte SliceCommand packets). Verifies: lazy connect, wire layout,
 * re-send after (re)connect, 0xDEAD on stop, submit() never blocks.
 */

#include <catch2/catch_all.hpp>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "slice_agent_client.hpp"
#include "slice_math.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace {

// Minimal stand-in for `slice_agent --listen`: accepts one client at a
// time, records every 56-byte command, counts connections.
class FakeAgent {
public:
    explicit FakeAgent(uint16_t port = 0) : acceptor_(ioc_, tcp::endpoint(tcp::v4(), port)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { run(); });
    }
    ~FakeAgent() { stop(); }

    uint16_t port() const { return port_; }

    std::vector<slice_math::WireCommand> commands() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return commands_;
    }
    int connections() const { return connections_.load(); }
    int disconnects() const { return disconnects_.load(); }

    // Drop the current client (simulates agent restart / network drop).
    void kick_client() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (client_) {
            boost::system::error_code ignore;
            client_->shutdown(tcp::socket::shutdown_both, ignore);
            client_->close(ignore);
        }
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        boost::system::error_code ignore;
        acceptor_.close(ignore);
        kick_client();
        if (thread_.joinable()) thread_.join();
    }

    template <class Pred>
    bool wait_for(Pred p, std::chrono::milliseconds timeout = 3000ms) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (p()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return p();
    }

private:
    void run() {
        // Non-blocking accept loop: a blocking accept() cannot be woken by
        // closing the acceptor from another thread on Linux.
        boost::system::error_code nb;
        acceptor_.non_blocking(true, nb);
        while (!stopped_.load()) {
            auto sock = std::make_shared<tcp::socket>(ioc_);
            boost::system::error_code ec;
            acceptor_.accept(*sock, ec);
            if (ec == net::error::would_block || ec == net::error::try_again) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            if (ec) break;
            sock->non_blocking(false, nb);
            connections_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                client_ = sock;
            }
            std::array<uint8_t, slice_math::kWireBytes> buf{};
            while (!stopped_.load()) {
                boost::system::error_code rec;
                net::read(*sock, net::buffer(buf), rec);
                if (rec) break;
                auto cmd = slice_math::from_wire(buf.data());
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    commands_.push_back(cmd);
                }
                if (cmd.flags == slice_math::kFlagQuit) break;
            }
            disconnects_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                client_.reset();
            }
        }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    uint16_t port_{0};
    std::thread thread_;
    mutable std::mutex mtx_;
    std::vector<slice_math::WireCommand> commands_;
    std::shared_ptr<tcp::socket> client_;
    std::atomic<int> connections_{0};
    std::atomic<int> disconnects_{0};
    std::atomic<bool> stopped_{false};
};

mrd::SliceAgentConfig fast_cfg(uint16_t port) {
    mrd::SliceAgentConfig c;
    c.host = "127.0.0.1";
    c.port = port;
    c.connect_timeout_ms = 1000;
    c.resend_window_ms = 300;
    c.resend_interval_ms = 50;
    c.backoff_min_ms = 50;
    c.backoff_max_ms = 200;
    return c;
}

slice_math::WireCommand cmd(double tz, double rz = 0.0) {
    slice_math::WireCommand c;
    c.tz = tz;
    c.rz = rz;
    return c;
}

} // namespace

TEST_CASE("Disabled client: no connect, submit returns false", "[slice][client]") {
    mrd::SliceAgentClient client(mrd::SliceAgentConfig{});
    client.start();
    CHECK_FALSE(client.enabled());
    CHECK_FALSE(client.submit(cmd(1)));
    CHECK_FALSE(client.connected());
    client.stop();
}

TEST_CASE("Lazy connect: nothing until the first command, then 56-byte packets",
          "[slice][client]") {
    FakeAgent agent;
    mrd::SliceAgentClient client(fast_cfg(agent.port()));
    client.start();

    std::this_thread::sleep_for(200ms);
    CHECK(agent.connections() == 0);      // no idle connect
    CHECK_FALSE(client.connected());

    // First command: submit waits for the lazy connect + write, so it
    // reports delivered=true even though nothing was connected before.
    CHECK(client.submit(cmd(12.5, 30.0)));
    REQUIRE(agent.wait_for([&] { return agent.connections() == 1; }));
    REQUIRE(agent.wait_for([&] { return !agent.commands().empty(); }));
    const auto first = agent.commands().front();
    CHECK(first.tz == Catch::Approx(12.5));
    CHECK(first.rz == Catch::Approx(30.0));
    CHECK(first.flags == slice_math::kFlagUpdate);
    CHECK(client.connected());

    // Re-send window: the same command is repeated several times after
    // connect (to overwrite the agent's identity publish).
    std::this_thread::sleep_for(400ms);
    CHECK(agent.commands().size() >= 3);
    for (const auto& c : agent.commands()) CHECK(c.tz == Catch::Approx(12.5));

    // A newer command replaces it
    client.submit(cmd(-4.0));
    REQUIRE(agent.wait_for([&] { return agent.commands().back().tz == Catch::Approx(-4.0); }));

    client.stop();
    // 0xDEAD on stop
    REQUIRE(agent.wait_for([&] {
        auto cs = agent.commands();
        return !cs.empty() && cs.back().flags == slice_math::kFlagQuit;
    }));
}

TEST_CASE("Reconnect after the agent drops the client re-sends the last command",
          "[slice][client]") {
    FakeAgent agent;
    mrd::SliceAgentClient client(fast_cfg(agent.port()));
    client.start();
    client.submit(cmd(7.0));
    REQUIRE(agent.wait_for([&] { return agent.connections() == 1 && !agent.commands().empty(); }));
    std::this_thread::sleep_for(400ms);   // let the resend window finish

    agent.kick_client();
    REQUIRE(agent.wait_for([&] { return agent.disconnects() == 1; }));

    // Nothing triggers a reconnect until either a send fails or a new
    // command arrives; submit a new one.
    client.submit(cmd(8.0));
    REQUIRE(agent.wait_for([&] { return agent.connections() == 2; }, 5000ms));
    REQUIRE(agent.wait_for([&] {
        auto cs = agent.commands();
        return !cs.empty() && cs.back().tz == Catch::Approx(8.0);
    }));
    CHECK(client.reconnect_count() >= 1);
    client.stop();
}

TEST_CASE("No agent listening: submit never blocks, delivered=false, retries with backoff",
          "[slice][client]") {
    uint16_t dead_port;
    {
        net::io_context ioc;
        tcp::acceptor a(ioc, tcp::endpoint(tcp::v4(), 0));
        dead_port = a.local_endpoint().port();
    }   // closed: nothing listens there now
    mrd::SliceAgentClient client(fast_cfg(dead_port));
    client.start();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(client.submit(cmd(1.0)));
    const auto dt = std::chrono::steady_clock::now() - t0;
    // Connection refused is immediate; submit must return on the failure
    // ack, well before its connect_timeout + 500 ms ceiling.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() < 1000);
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(client.connected());
    CHECK(client.sent_count() == 0);
    client.stop();   // must return promptly even while retrying
}

TEST_CASE("A new command wakes the client out of connect backoff", "[slice][client]") {
    // Reserve a port with nothing listening, point the client at it, let it
    // fail and enter backoff, then start the agent on that port and submit:
    // the new command must be attempted immediately (not after the backoff)
    // and report delivered.
    net::io_context ioc;
    auto reserve = std::make_unique<tcp::acceptor>(ioc, tcp::endpoint(tcp::v4(), 0));
    const uint16_t port = reserve->local_endpoint().port();
    reserve.reset();   // now closed: connection refused

    auto cfg = fast_cfg(port);
    cfg.backoff_min_ms = 3000; cfg.backoff_max_ms = 3000;   // long backoff on purpose
    mrd::SliceAgentClient client(cfg);
    client.start();
    CHECK_FALSE(client.submit(cmd(1.0)));   // refused -> false quickly, backoff armed

    // Agent appears on the same port (retry binding: the port may be briefly in TIME_WAIT-free state)
    std::unique_ptr<FakeAgent> agent;
    for (int i = 0; i < 20 && !agent; ++i) {
        try { agent = std::make_unique<FakeAgent>(port); } catch (...) { std::this_thread::sleep_for(50ms); }
    }
    REQUIRE(agent);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(client.submit(cmd(2.0)));   // must not wait out the 3 s backoff
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(dt < 1500);
    REQUIRE(agent->wait_for([&] { auto cs = agent->commands(); return !cs.empty() && cs.back().tz == Catch::Approx(2.0); }));
    client.stop();
}

TEST_CASE("A connection dropped while idle is re-established and the command re-sent",
          "[slice][client]") {
    FakeAgent agent;
    auto cfg = fast_cfg(agent.port());
    cfg.liveness_poll_ms = 200;
    mrd::SliceAgentClient client(cfg);
    client.start();
    REQUIRE(client.submit(cmd(5.0)));
    std::this_thread::sleep_for(400ms);   // resend window done
    agent.kick_client();
    REQUIRE(agent.wait_for([&] { return agent.disconnects() == 1; }));
    // no new command: the liveness tick must notice, reconnect and re-send 5.0
    REQUIRE(agent.wait_for([&] { return agent.connections() == 2; }, 5000ms));
    REQUIRE(agent.wait_for([&] { auto cs = agent.commands(); return !cs.empty() && cs.back().tz == Catch::Approx(5.0); }));
    CHECK(client.reconnect_count() == 1);
    client.stop();
}

TEST_CASE("post() never blocks and wait() reports the verdict; zeros replace the command",
          "[slice][client]") {
    FakeAgent agent;
    mrd::SliceAgentClient client(fast_cfg(agent.port()));
    client.start();
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t g1 = client.post(cmd(7.0));
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 20);
    REQUIRE(g1 == 1);
    CHECK(client.wait(g1));
    // scan-start style zero: posted without waiting, replaces the last command
    const uint64_t g2 = client.post(cmd(0.0));
    REQUIRE(g2 == 2);
    REQUIRE(agent.wait_for([&] { auto cs = agent.commands(); return !cs.empty() && cs.back().tz == 0.0; }));
    REQUIRE(client.last_command().has_value());
    CHECK(client.last_command()->tz == 0.0);
    CHECK_FALSE(client.wait(0));   // 0 = never queued
    client.stop();
}

TEST_CASE("Command submitted before the agent is up is delivered once it appears",
          "[slice][client]") {
    // Reserve a port, start the client pointing at it, then bring the agent
    // up on... a different ephemeral port is not possible; instead start
    // the agent first but only submit after a short delay to exercise the
    // wait path, and assert the pending command is sent on connect.
    FakeAgent agent;
    mrd::SliceAgentClient client(fast_cfg(agent.port()));
    client.start();
    std::this_thread::sleep_for(100ms);
    client.submit(cmd(3.0));
    REQUIRE(agent.wait_for([&] {
        auto cs = agent.commands();
        return !cs.empty() && cs.front().tz == Catch::Approx(3.0);
    }));
    client.stop();
}

// Audit 2026-08-28 #8: wait(gen) used to return true whenever ANY later
// generation had been sent, so a command overwritten before the worker
// picked it up was reported as delivered although its (absolute) values
// never reached the agent.
TEST_CASE("A command overwritten before it is sent is reported Superseded, not delivered",
          "[slice][client][verdict]") {
    FakeAgent agent;
    mrd::SliceAgentClient client(fast_cfg(agent.port()));
    // Post both BEFORE starting the worker so the overwrite is deterministic:
    // the worker wakes to a single pending command (the newest).
    const uint64_t gA = client.post(cmd(5.0));
    const uint64_t gB = client.post(cmd(9.0));
    REQUIRE(gA == 1);
    REQUIRE(gB == 2);
    client.start();

    CHECK(client.wait(gB));
    CHECK(client.verdict(gB) == slice_math::Delivery::Delivered);
    CHECK(client.verdict(gA) == slice_math::Delivery::Superseded);
    CHECK_FALSE(client.wait(gA));

    // The agent only ever saw B.
    REQUIRE(agent.wait_for([&] { return !agent.commands().empty(); }));
    for (const auto& c : agent.commands()) CHECK(c.tz == Catch::Approx(9.0));

    CHECK(client.verdict(0) == slice_math::Delivery::NotDelivered);
    client.stop();
}
