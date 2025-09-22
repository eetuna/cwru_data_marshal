#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace websocket = boost::beast::websocket;

// add near the top, after includes
static std::time_t file_time_to_time_t(std::filesystem::file_time_type t)
{
    using namespace std::chrono;
    // Convert file_clock -> system_clock
    const auto sctp = time_point_cast<system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

int main(int argc, char **argv)
{
    std::string ws_url = "ws://localhost:8090/ws";
    std::string data = "./data/mrd"; // sensible default

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--ws" && i + 1 < argc)
            ws_url = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data = argv[++i];
    }

    fs::path latest = fs::path(data) / "latest.json";
    if (!fs::exists(latest))
    {
        std::cerr << "viz: waiting for latest.json...\n";
        while (!fs::exists(latest))
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- Poll latest.json forever ---
    std::thread poll([&]()
                     {
        std::time_t last = 0;
        while (true) {
            try {
                if (fs::exists(latest)) {
                   // auto wt = decltype(fs::last_write_time(latest))::clock::to_time_t(fs::last_write_time(latest));
                   auto wt = file_time_to_time_t(fs::last_write_time(latest));
                   if (wt != last) {
                        std::ifstream lf(latest);
                        json lj; lf >> lj;
                        std::cout << "viz latest=" << lj.dump() << "\n";
                        last = wt;
                    }
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } });

    // --- Connect WS once, print until closed ---
    try
    {
        boost::asio::io_context ioc;
        auto pos = ws_url.find("//");
        auto hp = ws_url.substr(pos + 2);
        auto slash = hp.find("/");
        std::string host = hp.substr(0, hp.find(":"));
        std::string port = hp.substr(host.size() + 1, slash - host.size() - 1);
        std::string target = hp.substr(slash);

        boost::asio::ip::tcp::resolver res{ioc};
        auto results = res.resolve(host, port);
        boost::asio::ip::tcp::socket sock{ioc};
        boost::asio::connect(sock, results.begin(), results.end());
        websocket::stream<boost::asio::ip::tcp::socket> ws{std::move(sock)};
        ws.handshake(host + ":" + port, target);

        std::cout << "viz: WS connected " << ws_url << "\n";
        boost::beast::flat_buffer buf;
        while (true)
        {
            boost::system::error_code ec;
            ws.read(buf, ec);
            if (ec)
            {
                std::cerr << "viz: WS closed: " << ec.message() << "\n";
                break; // just exit WS loop, poll thread still runs
            }
            auto s = boost::beast::buffers_to_string(buf.data());
            buf.consume(buf.size());
            auto j = json::parse(s, nullptr, false);
            if (j.is_object())
                std::cout << "viz ws: " << j.dump() << "\n";
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "viz: WS error: " << e.what() << "\n";
    }

    poll.join();
    return 0;
}
