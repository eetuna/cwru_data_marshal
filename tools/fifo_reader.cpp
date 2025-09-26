#include <iostream>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main() {
    std::string fifo = std::getenv("DM_FIFO") ? std::getenv("DM_FIFO") : std::string("./data/fifos/mrd_events.fifo");
    std::filesystem::create_directories(std::filesystem::path(fifo).parent_path());
    struct stat st{};
    if (stat(fifo.c_str(), &st) != 0) {
        if (mkfifo(fifo.c_str(), 0666) != 0) { perror("mkfifo"); return 1; }
    }
    std::cout << "[FIFO-READER] opening " << fifo << "...\n";
    int fd = ::open(fifo.c_str(), O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    std::string buf; buf.resize(4096);
    while (true) {
        ssize_t n = ::read(fd, buf.data(), (ssize_t)buf.size());
        if (n <= 0) break;
        std::cout << "[FIFO-READER] " << std::string(buf.data(), (size_t)n);
        std::cout.flush();
    }
    ::close(fd);
    return 0;
}
