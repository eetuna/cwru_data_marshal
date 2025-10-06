#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string envs(const char *k, const std::string &def)
{
    const char *v = std::getenv(k);
    return v ? std::string(v) : def;
}

int main()
{
    const std::string index_path = envs("DM_INDEX", "./data/mrd/index.jsonl");
    const std::string offset_path = envs("DM_TAIL_OFFSET", "./data/mrd/.fs_tail_offset");

    fs::create_directories(fs::path(index_path).parent_path());
    if (!fs::exists(index_path))
    {
        std::ofstream make(index_path);
        make.close();
    }

    std::uint64_t offset = 0;
    if (fs::exists(offset_path))
    {

        try
        {
            std::ifstream in(offset_path);
            in >> offset;
        }
        catch (...)
        {
        }
    }
    std::ifstream in(index_path, std::ios::in);
    if (!in)
    {
        std::cerr << "[FS-TAIL] cannot open index: " << index_path << "\n";
        return 1;
    }
    in.seekg((std::streamoff)offset);

    std::string line;
    while (true)
    {
        if (!std::getline(in, line))
        {
            if (in.eof())
            {
                in.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            in.clear();
            continue;
        }
        // persist offset
        std::uint64_t off = (std::uint64_t)in.tellg();
        {
            std::ofstream out(offset_path, std::ios::trunc);
            out << off;
        }

        if (line.empty())
            continue;
        try
        {
            auto rec = json::parse(line);
            std::string path = rec.value("path", "");
            if (path.empty())
            {
                std::cerr << "[FS-TAIL] entry missing path\n";
                continue;
            }
            if (!fs::exists(path))
            {
                std::cerr << "[FS-TAIL] not found yet: " << path << "\n";
                continue;
            }
            auto sz = fs::file_size(path);
            std::cout << "[FS-TAIL] MRD ready: " << path << " (" << sz << " bytes)\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "[FS-TAIL] bad index line: " << e.what() << "\n";
        }
    }
    return 0;
}
