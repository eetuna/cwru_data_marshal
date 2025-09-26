#pragma once
#include <filesystem>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

struct FifoSink {
    std::filesystem::path fifo_path;
    std::atomic<bool> running{false};
    std::thread worker;
    int fd{-1};
    std::mutex mtx;

    explicit FifoSink(std::filesystem::path p) : fifo_path(std::move(p)) {}
    void start();
    void stop();
    bool write_line(const std::string& jsonl);
private:
    void open_fifo_loop();
};
