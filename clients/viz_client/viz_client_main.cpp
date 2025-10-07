#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <hdf5.h>
#include <vector>
#include <algorithm>
#include <limits>

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

static void render_ascii(const std::vector<float> &voxels,
                         size_t nx,
                         size_t ny,
                         size_t nz,
                         size_t frame_idx)
{
    static const std::string palette = " .:-=+*#%@";
    auto [min_it, max_it] = std::minmax_element(voxels.begin(), voxels.end());
    float min_v = (min_it != voxels.end()) ? *min_it : 0.f;
    float max_v = (max_it != voxels.end()) ? *max_it : 1.f;
    float span = std::max(1e-6f, max_v - min_v);

    std::cout << "\n=== Frame " << frame_idx + 1 << " (" << nz << " slices) ===\n";
    for (size_t z = 0; z < nz; ++z)
    {
        std::cout << "Slice " << (z + 1) << "/" << nz << "\n";
        for (size_t y = 0; y < ny; ++y)
        {
            std::string line;
            line.reserve(nx);
            for (size_t x = 0; x < nx; ++x)
            {
                size_t idx = z * ny * nx + y * nx + x;
                float norm = (voxels[idx] - min_v) / span;
                size_t palette_idx = static_cast<size_t>(norm * (palette.size() - 1));
                if (palette_idx >= palette.size())
                    palette_idx = palette.size() - 1;
                line.push_back(palette[palette_idx]);
            }
            std::cout << line << '\n';
        }
        std::cout << '\n';
    }
    std::cout.flush();
}

static void inspect_swmr_file(const std::string &path)
{
    if (path.empty())
        return;
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0)
        return;
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    if (file < 0)
    {
        std::cerr << "viz: unable to open SWMR file " << path << "\n";
        return;
    }
    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    if (dset < 0)
    {
        std::cerr << "viz: /images/data dataset missing in " << path << "\n";
        H5Fclose(file);
        return;
    }
    if (H5Drefresh(dset) < 0)
    {
        std::cerr << "viz: H5Drefresh failed for " << path << "\n";
    }
    hid_t space = H5Dget_space(dset);
    if (space < 0)
    {
        std::cerr << "viz: cannot read dataset space" << std::endl;
        H5Dclose(dset);
        H5Fclose(file);
        return;
    }
    int rank = H5Sget_simple_extent_ndims(space);
    std::vector<hsize_t> dims(rank, 0);
    if (rank > 0)
        H5Sget_simple_extent_dims(space, dims.data(), nullptr);

    if (dims.size() == 5 && dims[0] > 0)
    {
        const hsize_t frames = dims[0];
        const hsize_t channels = dims[1];
        const hsize_t nz = dims[2];
        const hsize_t ny = dims[3];
        const hsize_t nx = dims[4];

        hsize_t mem_dims[5] = {1, 1, nz, ny, nx};
        hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
        if (memspace >= 0)
        {
            std::vector<float> voxels(static_cast<size_t>(nz * ny * nx));
            hsize_t start[5] = {frames - 1, 0, 0, 0, 0};
            hsize_t count[5] = {1, 1, nz, ny, nx};
            if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr) >= 0)
            {
                if (channels > 1)
                {
                    std::cerr << "viz: only first channel rendered (" << channels << " available)\n";
                }
                if (H5Dread(dset, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT, voxels.data()) >= 0)
                {
                    render_ascii(voxels, nx, ny, nz, frames - 1);
                }
                else
                {
                    std::cerr << "viz: H5Dread failed for " << path << "\n";
                }
            }
            H5Sclose(memspace);
        }
        std::cout << "viz swmr: frames=" << frames
                  << " channels=" << channels
                  << " shape=" << nx << "x" << ny << "x" << nz
                  << "\n";
    }

    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);
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
                        if (lj.contains("frame_index") && lj.contains("path"))
                        {
                            try
                            {
                                inspect_swmr_file(lj["path"].get<std::string>());
                            }
                            catch (...)
                            {
                            }
                        }
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
            {
                std::cout << "viz ws: " << j.dump() << "\n";
                if (j.contains("frame_index") && j.contains("path"))
                {
                    try
                    {
                        inspect_swmr_file(j["path"].get<std::string>());
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "viz: WS error: " << e.what() << "\n";
    }

    poll.join();
    return 0;
}
