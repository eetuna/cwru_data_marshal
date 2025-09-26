#include "sinks/fifo_sink.hpp"
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>

void FifoSink::start() {
    if (running.exchange(true)) return;
    struct stat st{};
    if (stat(fifo_path.c_str(), &st) != 0) {
        if (mkfifo(fifo_path.c_str(), 0666) != 0) {
            throw std::runtime_error(std::string("mkfifo failed: ") + std::strerror(errno));
        }
    } else if (!S_ISFIFO(st.st_mode)) {
        throw std::runtime_error("path exists but not FIFO: " + fifo_path.string());
    }
    worker = std::thread(&FifoSink::open_fifo_loop, this);
}

void FifoSink::stop() {
    if (!running.exchange(false)) return;
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> _g(mtx);
    if (fd >= 0) { ::close(fd); fd = -1; }
}

void FifoSink::open_fifo_loop() {
    while (running.load()) {
        {
            std::lock_guard<std::mutex> _g(mtx);
            if (fd < 0) {
                fd = ::open(fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool FifoSink::write_line(const std::string& jsonl) {
    std::lock_guard<std::mutex> _g(mtx);
    if (fd < 0) return false;
    std::string with_nl = jsonl; with_nl.push_back('\n');
    ssize_t n = ::write(fd, with_nl.data(), (ssize_t)with_nl.size());
    if (n < 0) {
        if (errno == EPIPE) { ::close(fd); fd = -1; }
        return false;
    }
    return (size_t)n == with_nl.size();
}
