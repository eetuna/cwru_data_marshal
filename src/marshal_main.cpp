#include <iostream>
#include <boost/asio.hpp>
#include "marshal_http.hpp"
#include "marshal_ws.hpp"
#include "marshal_state.hpp"

int main(int argc, char **argv)
{
    std::string http_bind = "0.0.0.0:8080";
    std::string ws_bind = "0.0.0.0:8090";
    std::string data_dir = "/data";
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--http" && i + 1 < argc)
            http_bind = argv[++i];
        else if (a == "--ws" && i + 1 < argc)
            ws_bind = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data_dir = argv[++i];
    }
    auto split = [](const std::string &s)
    { auto p=s.find(":"); return std::pair{s.substr(0,p), static_cast<unsigned short>(std::stoi(s.substr(p+1)))}; };
    auto [http_host, http_port] = split(http_bind);
    auto [ws_host, ws_port] = split(ws_bind);

    boost::asio::io_context ioc{1};
    MarshalState state;
    state.io = &ioc;

    boost::asio::ip::tcp::endpoint http_ep{boost::asio::ip::make_address(http_host), http_port};
    boost::asio::ip::tcp::endpoint ws_ep{boost::asio::ip::make_address(ws_host), ws_port};

    HttpServer http{ioc, http_ep, state};
    WsServer ws{ioc, ws_ep, state};

    std::cout << "marshal listening http=" << http_bind << " ws=" << ws_bind << "\n";
    ioc.run();
    return 0;
}