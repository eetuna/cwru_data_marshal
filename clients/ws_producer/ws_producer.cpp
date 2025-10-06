// Minimal C++ WS producer that streams full MRD payloads over WebSocket.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

int main()
{
  std::string host = std::getenv("WS_HOST") ? std::getenv("WS_HOST") : "localhost";
  std::string port = std::getenv("WS_PORT") ? std::getenv("WS_PORT") : "8090";
  std::string target = std::getenv("WS_TARGET") ? std::getenv("WS_TARGET") : "/";
  int frames = std::getenv("WS_FRAMES") ? std::atoi(std::getenv("WS_FRAMES")) : 10;
  size_t bytes = std::getenv("WS_BYTES") ? std::strtoull(std::getenv("WS_BYTES"), nullptr, 10) : 65536;
  std::string file = std::getenv("WS_FILE") ? std::getenv("WS_FILE") : "";

  try
  {
    asio::io_context ioc;
    tcp::resolver res{ioc};
    auto results = res.resolve(host, port);
    beast::tcp_stream s{ioc};
    s.connect(results);
    websocket::stream<beast::tcp_stream> ws{std::move(s)};
    ws.handshake(host, target);

    std::vector<uint8_t> payload;
    if (!file.empty())
    {
      std::ifstream in(file, std::ios::binary);
      if (!in)
      {
        std::cerr << "cannot open WS_FILE=" << file << "\n";
        return 1;
      }
      payload.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    else
    {
      payload.assign(bytes, 0x42);
    }
    if (payload.empty())
    {
      std::cerr << "payload is empty" << std::endl;
      return 1;
    }

    ws.text(false);
    for (int i = 1; i <= frames; i++)
    {
      ws.binary(true);
      ws.write(asio::buffer(payload));
      beast::flat_buffer buf;
      ws.read(buf);
      std::cout << beast::make_printable(buf.data()) << std::endl;
    }

    ws.close(websocket::close_code::normal);
  }
  catch (const std::exception &e)
  {
    std::cerr << "ws_producer error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
