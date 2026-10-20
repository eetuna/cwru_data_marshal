/*
 * tests/test_scanner_race.cpp
 * Scanner connections are served one at a time, like python-ismrmrd-server:
 * a second connection waits (it is neither rejected nor allowed to disturb
 * the first) and is served once the first scan has ended. With no recon
 * reachable, each scan still finalizes: RECON FAILED image, then CLOSE,
 * then the marshal closes the connection.
 */

#include <catch2/catch_all.hpp>

#include <boost/asio.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
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

void init_state(MarshalState& state) {
    state.dump_dir = std::filesystem::temp_directory_path()
                   / ("test_scanner_race_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(state.dump_dir);
    state.dump_recorder = std::make_unique<mrd::DumpRecorder>(state.dump_dir);
    state.snapshot = std::make_unique<mrd::LatestSnapshot>(state.dump_dir / "latest");
}

void send_close(tcp::socket& s) {
    const uint16_t tag = mrd::MRD_MESSAGE_CLOSE;
    net::write(s, net::buffer(&tag, 2));
}

// Read message tags until EOF; returns them in order (bodies skipped by
// length for IMAGE, the only message with a body the marshal sends here).
std::vector<uint16_t> read_until_eof(tcp::socket& s) {
    std::vector<uint16_t> tags;
    for (;;) {
        uint16_t tag = 0;
        boost::system::error_code ec;
        net::read(s, net::buffer(&tag, 2), ec);
        if (ec) break;
        tags.push_back(tag);
        if (tag == mrd::MRD_MESSAGE_ISMRMRD_IMAGE) {
            ISMRMRD::ImageHeader h;
            uint64_t attr_len = 0;
            net::read(s, net::buffer(&h, mrd::IMAGE_HEADER_BYTES), ec);
            net::read(s, net::buffer(&attr_len, 8), ec);
            std::vector<uint8_t> rest(attr_len + size_t(h.matrix_size[0]) * h.matrix_size[1]
                                      * h.matrix_size[2] * h.channels
                                      * ISMRMRD::ismrmrd_sizeof_data_type(h.data_type));
            net::read(s, net::buffer(rest), ec);
            if (ec) break;
        }
    }
    return tags;
}
} // namespace

TEST_CASE("Second scanner waits its turn; each scan finalizes without recon",
          "[mrd][race]") {
    MarshalState state;
    init_state(state);

    net::io_context ioc;
    uint16_t port = get_ephemeral_port();
    mrd::MrdTcpListener listener(ioc, port, state, nullptr);
    std::thread ioc_thread([&] { ioc.run(); });

    tcp::socket scanner1(ioc);
    scanner1.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(listener.has_scanner());

    // Second scanner connects while the first scan runs: it is kept waiting,
    // not closed.
    tcp::socket scanner2(ioc);
    scanner2.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        boost::system::error_code ec;
        char b;
        scanner2.non_blocking(true);
        scanner2.read_some(net::buffer(&b, 1), ec);
        CHECK(ec == net::error::would_block);   // open and silent
        scanner2.non_blocking(false);
    }

    // First scan ends: RECON FAILED image, CLOSE, then the marshal closes.
    send_close(scanner1);
    auto tags1 = read_until_eof(scanner1);
    REQUIRE(tags1.size() == 2);
    CHECK(tags1[0] == mrd::MRD_MESSAGE_ISMRMRD_IMAGE);
    CHECK(tags1[1] == mrd::MRD_MESSAGE_CLOSE);

    // Now the waiting scanner is served the same way.
    send_close(scanner2);
    auto tags2 = read_until_eof(scanner2);
    REQUIRE(tags2.size() == 2);
    CHECK(tags2[1] == mrd::MRD_MESSAGE_CLOSE);

    CHECK(state.get_status().first == "recon failed");

    listener.stop();
    ioc.stop();
    if (ioc_thread.joinable()) ioc_thread.join();
}
