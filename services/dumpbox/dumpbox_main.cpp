#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <ismrmrd/dataset.h>
#include <ismrmrd/ismrmrd.h>
#include <boost/beast/core/flat_buffer.hpp>

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

    // connect WS
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver res{ioc};
    auto pos = ws_url.find("//");
    auto hostport = ws_url.substr(pos + 2);
    auto slash = hostport.find("/");
    auto host = hostport.substr(0, hostport.find(":"));
    auto port = hostport.substr(host.size() + 1, slash - host.size() - 1);
    auto target = hostport.substr(slash);
    auto const results = res.resolve(host, port);
    boost::asio::ip::tcp::socket sock{ioc};
    boost::asio::connect(sock, results.begin(), results.end());
    websocket::stream<boost::asio::ip::tcp::socket> ws{std::move(sock)};
    ws.handshake(host, target);
    //std::string host_hdr = host + ":" + port; // many servers expect host:port
    //ws.handshake(host_hdr, target);
    //ws.text(true); // we expect JSON text

    // prepare MRD path
    auto day = fs::path(data) / "mrd";
    fs::create_directories(day);
    auto file = day / "run_00001.h5"; // rotate later
    ISMRMRD::Dataset d(file.string().c_str(), "dataset", true);

    auto index_path = fs::path(data) / "index.jsonl";
    auto latest_path = fs::path(data) / "latest.json";

    // receive and append loop
    boost::beast::flat_buffer buffer;
    while (true)
    {
        ws.read(buffer);
        auto s = boost::beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        auto j = json::parse(s, nullptr, false);
        if (!j.is_object() || !j.contains("topic"))
            continue;
        if (j["topic"] == "mrd.acq")
        {
            // toy example: write a dummy acquisition with timestamp meta
            ISMRMRD::Acquisition acq;
            acq.resize(/*num_samples*/ 1, /*active_channels*/ 1, /*traj_dims*/ 0);
            acq.sample_time_us() = 1000.0f;      // microseconds
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            d.appendAcquisition(acq);
            // index.jsonl append
            std::ofstream idx(index_path, std::ios::app);
            idx << json{{"t_ms", ms}, {"file", file.string()}, {"type", "acq"}}.dump() << "\n";
            // latest.json atomic update
            std::ofstream lat(latest_path, std::ios::trunc);
            lat << json{{"file", file.string()}, {"updated_ms", ms}}.dump();
        }
    }
}