#pragma once
#include <filesystem>
#include <mutex>
#include <string>

struct IndexWriter {
    std::filesystem::path index_path;
    std::mutex mtx;
    explicit IndexWriter(std::filesystem::path p) : index_path(std::move(p)) {}
    void append_line(const std::string& jsonl);
};
