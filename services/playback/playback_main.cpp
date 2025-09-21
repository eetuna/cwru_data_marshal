/*
 * HTTP-only playback that re-POSTs MRDs from a dumpbox session to /v1/mrd/ingest
 */
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using json = nlohmann::json;
using tcp = asio::ip::tcp;

static void parse_http_url(const std::string &url, std::string &host, std::string &port, std::string &base_target)
{
    if (url.rfind("http://", 0) != 0)
        throw std::runtime_error("only http:// supported");
    std::string rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    base_target = slash == std::string::npos ? "" : rest.substr(slash);
    auto colon = hostport.find(':');
    if (colon == std::string::npos)
    {
        host = hostport;
        port = "80";
    }
    else
    {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon + 1);
    }
}

static void http_post_file(const std::string &http_base, const std::string &file_path)
{
    std::string host, port, base_target;
    parse_http_url(http_base, host, port, base_target);
    std::string target = "/v1/mrd/ingest";
    if (!base_target.empty() && base_target != "/")
    {
        target = base_target.back() == '/' ? (base_target + "v1/mrd/ingest")
                                           : (base_target + "/v1/mrd/ingest");
    }

    asio::io_context ioc;
    tcp::resolver resolver{ioc};
    auto const results = resolver.resolve(host, port);
    tcp::socket socket{ioc};
    asio::connect(socket, results.begin(), results.end());

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs)
        throw std::runtime_error("open failed: " + file_path);
    std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::content_type, "application/octet-stream");
    req.body() = std::move(body);
    req.prepare_payload();

    http::response<http::string_body> res;
    beast::flat_buffer buf;
    http::write(socket, req);
    http::read(socket, buf, res);
    beast::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
    if (res.result() != http::status::ok &&
        res.result() != http::status::no_content &&
        res.result() != http::status::created)
    {
        throw std::runtime_error("HTTP POST failed: " + std::to_string((int)res.result()));
    }
}

static std::chrono::system_clock::time_point parse_iso_loose(const std::string &s)
{
    std::tm tm{};
    long ms = 0;
    if (s.size() >= 20)
    {
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2));
        tm.tm_hour = std::stoi(s.substr(11, 2));
        tm.tm_min = std::stoi(s.substr(14, 2));
        tm.tm_sec = std::stoi(s.substr(17, 2));
        auto dot = s.find('.', 19);
        if (dot != std::string::npos)
            ms = std::stol(s.substr(dot + 1));
    }
    auto tt = timegm(&tm);
    return std::chrono::system_clock::from_time_t(tt) + std::chrono::milliseconds(ms);
}

int main(int argc, char **argv)
{
    try
    {
        std::string http_base = "http://localhost:8080";
        fs::path session_dir = "./data/dumpbox";
        double speed = 0.0;

        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--http" && i + 1 < argc)
                http_base = argv[++i];
            else if (a == "--data" && i + 1 < argc)
                session_dir = argv[++i];
            else if (a == "--speed" && i + 1 < argc)
                speed = std::stod(argv[++i]);
        }

        fs::path index_path = session_dir / "index.jsonl";
        if (!fs::exists(index_path))
        {
            std::cerr << "index.jsonl not found under --data: " << index_path << "\n";
            return 1;
        }

        struct Item
        {
            std::chrono::system_clock::time_point ts;
            fs::path file;
        };
        std::vector<Item> items;

        {
            std::ifstream ifs(index_path);
            std::string line;
            while (std::getline(ifs, line))
            {
                if (line.empty())
                    continue;
                try
                {
                    auto j = json::parse(line);
                    if (!j.contains("file") && !j.contains("path"))
                        continue;

                    const std::string ts_str = j.value("ts", "");
                    std::string path_str = j.contains("file")
                                               ? j["file"].get<std::string>()
                                               : j["path"].get<std::string>();

                    // Normalize entry:
                    //  - absolute paths: use as-is
                    //  - relative paths:
                    //      * if they already contain "files/", trim to "files/…"
                    //      * then join with session_dir
                    fs::path p_final;
                    fs::path p_raw(path_str);

                    if (p_raw.is_absolute())
                    {
                        p_final = p_raw;
                    }
                    else
                    {
                        // strip leading "./"
                        if (path_str.rfind("./", 0) == 0)
                            path_str.erase(0, 2);
                        // if the string embeds the session path, strip everything before "files/"
                        auto pos = path_str.find("files/");
                        std::string rel = (pos != std::string::npos) ? path_str.substr(pos)
                                                                     : path_str;
                        p_final = session_dir / rel;
                    }

                    auto ts = ts_str.empty() ? std::chrono::system_clock::now() : parse_iso_loose(ts_str);
                    items.push_back({ts, p_final});
                }
                catch (...)
                {
                    // skip malformed line
                }
            }
        }

        if (items.empty())
        {
            std::cerr << "no items in index.jsonl\n";
            return 1;
        }

        std::cout << "playback (HTTP): " << items.size()
                  << " files -> " << http_base << "/v1/mrd/ingest\n";

        auto t0 = items.front().ts;
        auto last_waited = std::chrono::milliseconds(0);

        for (size_t i = 0; i < items.size(); ++i)
        {
            auto &it = items[i];
            if (speed > 0.0)
            {
                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>((it.ts - t0) / speed);
                if (dt > last_waited)
                {
                    std::this_thread::sleep_for(dt - last_waited);
                    last_waited = dt;
                }
            }
            if (!fs::exists(it.file))
            {
                std::cerr << "missing file: \"" << it.file.string() << "\"\n";
                continue;
            }
            http_post_file(http_base, it.file.string());
            std::cout << "posted " << it.file.filename().string()
                      << " (" << (i + 1) << "/" << items.size() << ")\n";
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "playback error: " << e.what() << "\n";
        return 1;
    }
}
