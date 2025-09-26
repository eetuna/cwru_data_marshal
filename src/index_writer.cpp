#include "index_writer.hpp"
#include "atomic_append.hpp"

void IndexWriter::append_line(const std::string& jsonl) {
    std::lock_guard<std::mutex> _g(mtx);
    atomic_append_line(index_path, jsonl);
}
