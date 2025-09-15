#include <iostream>
#include <filesystem>
#include <thread>
#include <stdexcept>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <ismrmrd/dataset.h>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace websocket = boost::beast::websocket;

static void ws_send(websocket::stream<boost::asio::ip::tcp::socket> &ws, const json &j)
{
    const std::string s = j.dump();
    ws.text(true);
    ws.write(boost::asio::buffer(s));
}

static void parse_ws_url(const std::string &ws_url,
                         std::string &host, std::string &port, std::string &target)
{
    // expect ws://host:port/path
    auto scheme_pos = ws_url.find("://");
    auto rest = (scheme_pos == std::string::npos) ? ws_url : ws_url.substr(scheme_pos + 3);
    auto slash = rest.find('/');
    std::string hp = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    target = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = hp.find(':');
    if (colon == std::string::npos)
    {
        host = hp;
        port = "80"; // default
    }
    else
    {
        host = hp.substr(0, colon);
        port = hp.substr(colon + 1);
    }
}

int main(int argc, char **argv)
{
    try
    {
        std::string http = "http://localhost:8080"; // reserved for future
        std::string ws_url = "ws://localhost:8090/ws";
        std::string data = "/data"; // NOTE: if your metadata lives under /data/mrd, pass --data /data/mrd

        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--http" && i + 1 < argc)
                http = argv[++i];
            else if (a == "--ws" && i + 1 < argc)
                ws_url = argv[++i];
            else if (a == "--data" && i + 1 < argc)
                data = argv[++i];
        }

        const fs::path latest = fs::path(data) / "latest.json";
        if (!fs::exists(latest))
            std::cerr << "no latest.json; waiting...\n";
        while (!fs::exists(latest))
            std::this_thread::sleep_for(std::chrono::seconds(1));

        // parse latest.json
        json lj;
        {
            std::ifstream lf(latest);
            if (!lf)
                throw std::runtime_error("failed to open latest.json at " + latest.string());
            lf >> lj;
        }

        // Use "path" (server writes this); fall back to "file" if present
        std::string mrd_path = lj.value("path", std::string());
        if (mrd_path.empty())
            mrd_path = lj.value("file", std::string());
        if (mrd_path.empty())
            throw std::runtime_error("latest.json missing both 'path' and 'file'");

        // (Optional) validate type/seq if you want:
        // std::string typ = lj.value("type", "");
        // if (typ.empty()) throw std::runtime_error("latest.json missing 'type'");

        // open MRD dataset
        ISMRMRD::Dataset d(mrd_path.c_str(), "dataset", false);
        const uint64_t n = d.getNumberOfAcquisitions();
        std::cerr << "Acquisitions: " << n << "\n";

        // connect WS once
        boost::asio::io_context ioc;
        std::string host, port, target;
        parse_ws_url(ws_url, host, port, target);

        boost::asio::ip::tcp::resolver res{ioc};
        auto const results = res.resolve(host, port);
        boost::asio::ip::tcp::socket sock{ioc};
        boost::asio::connect(sock, results.begin(), results.end());
        websocket::stream<boost::asio::ip::tcp::socket> ws{std::move(sock)};
        ws.handshake(host, target); // Host header is just host (no port) which is fine for local
        std::cerr << "WebSocket connected to " << ws_url << "\n";

        // naive pacing: one-by-one with small delay
        for (uint64_t i = 0; i < n; ++i)
        {
            ISMRMRD::Acquisition acq;
            d.readAcquisition(i, acq);
            ws_send(ws, json{
                            {"topic", "mrd.acq"},
                            {"payload", {{"idx", i}}}});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (i % 100 == 0 || i + 1 == n)
            {
                std::cerr << "Sent " << (i + 1) << "/" << n << "\n";
            }
        }

        // close politely
        ws.close(websocket::close_code::normal);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "playback error: " << e.what() << "\n";
        return 1;
    }
}
