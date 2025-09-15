#include <iostream>
#include <filesystem>
#include <fstream>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace websocket = boost::beast::websocket;

int main(int argc, char **argv)
{
    std::string ws_url = "ws://localhost:8090/ws";
    std::string data = "/data";
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--ws" && i + 1 < argc)
            ws_url = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data = argv[++i];
    }

    // watch latest.json (polling)
    auto latest = fs::path(data) / "latest.json";
    if (!fs::exists(latest))
        std::cerr << "viz: waiting for latest.json...\n";
    while (!fs::exists(latest))
        std::this_thread::sleep_for(std::chrono::seconds(1));
    std::ifstream lf(latest);
    json lj;
    lf >> lj;
    std::cout << "viz: latest=" << lj.dump() << "\n";

    // connect WS and print incoming pose/acq
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver res{ioc};
    auto pos = ws_url.find("//");
    auto hp = ws_url.substr(pos + 2);
    auto slash = hp.find("/");
    auto host = hp.substr(0, hp.find(":"));
    auto port = hp.substr(host.size() + 1, slash - host.size() - 1);
    auto target = hp.substr(slash);
    auto const results = res.resolve(host, port);
    boost::asio::ip::tcp::socket sock{ioc};
    boost::asio::connect(sock, results.begin(), results.end());
    websocket::stream<boost::asio::ip::tcp::socket> ws{std::move(sock)};
    ws.handshake(host, target);
    boost::beast::flat_buffer buf;
    while (true)
    {
        ws.read(buf);
        auto s = boost::beast::buffers_to_string(buf.data());
        buf.consume(buf.size());
        auto j = json::parse(s, nullptr, false);
        if (j.is_object())
            std::cout << "viz got: " << j.dump() << "\n";
    }
}