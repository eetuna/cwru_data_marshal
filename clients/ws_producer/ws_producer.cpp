// Minimal C++ WS producer with per-message metadata header.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

static std::vector<uint8_t> make_hdr(uint64_t frame_idx, uint64_t ts_ns,
                                     const std::string &series, uint64_t payload_len,
                                     uint16_t flags = 0, uint16_t ver = 1)
{
  std::vector<uint8_t> v;
  v.reserve(32 + series.size());
  auto push16 = [&](uint16_t x)
  { v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); };
  auto push64 = [&](uint64_t x)
  { for (int i=0;i<8;i++){ v.push_back(uint8_t(x>>(8*i))); } };
  v.insert(v.end(), {'M', 'R', 'D', '1'});
  push16(ver);
  push16(flags);
  push16((uint16_t)series.size());
  push16(0);
  push64(frame_idx);
  push64(ts_ns);
  push64(payload_len);
  v.insert(v.end(), series.begin(), series.end());
  return v;
}

int main()
{
  std::string host = std::getenv("WS_HOST") ? std::getenv("WS_HOST") : "localhost";
  std::string port = std::getenv("WS_PORT") ? std::getenv("WS_PORT") : "8090";
  std::string target = std::getenv("WS_TARGET") ? std::getenv("WS_TARGET") : "/ws";
  std::string series = std::getenv("WS_SERIES") ? std::getenv("WS_SERIES") : "T1rt";
  int frames = std::getenv("WS_FRAMES") ? std::atoi(std::getenv("WS_FRAMES")) : 10;
  size_t bytes = std::getenv("WS_BYTES") ? std::strtoull(std::getenv("WS_BYTES"), nullptr, 10) : 65536;

  try
  {
    asio::io_context ioc;
    tcp::resolver res{ioc};
    auto results = res.resolve(host, port);
    beast::tcp_stream s{ioc};
    s.connect(results);
    websocket::stream<beast::tcp_stream> ws{std::move(s)};
    ws.handshake(host, target);

    // Enter ingest mode by control message (must be UTF-8 text)
    ws.text(true);
    ws.write(asio::buffer(std::string("INGEST series=" + series + " topic=mrd.acq")));

    // Now send binary frames
    ws.text(false);

    std::vector<uint8_t> payload(bytes, 0x42);
    for (int i = 1; i <= frames; i++)
    {
      uint64_t ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      auto hdr = make_hdr(i, ts_ns, series, payload.size());
      std::vector<uint8_t> msg;
      msg.reserve(hdr.size() + payload.size());
      msg.insert(msg.end(), hdr.begin(), hdr.end());
      msg.insert(msg.end(), payload.begin(), payload.end());
      ws.binary(true);
      ws.write(asio::buffer(msg));
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
