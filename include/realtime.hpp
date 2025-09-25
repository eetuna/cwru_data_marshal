#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <span>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <unordered_map>

struct FrameMeta
{
  std::string series;
  uint64_t frame_idx{0};
  uint64_t ts_ns{0};
  uint32_t bytes{0};
  // For on-disk discovery:
  std::string path; // filled after write
  uint64_t offset{0};
};

struct Frame
{
  FrameMeta meta;
  std::vector<uint8_t> payload; // simple & safe; can be replaced with a pool later
};

inline Frame make_frame(std::string series, uint64_t frame_idx, uint64_t ts_ns,
                        std::vector<uint8_t> body)
{
  Frame f;
  f.meta.series = std::move(series);
  f.meta.frame_idx = frame_idx;
  f.meta.ts_ns = ts_ns;
  f.payload = std::move(body);
  return f;
};
// Minimal bounded MPSC queue using a mutex+cv (simple & robust).
// If you want lock-free later, you can swap it; API stays the same.
class FrameQueue
{
public:
  explicit FrameQueue(size_t cap) : cap_(cap) {}

  // Producer: try to enqueue; drops oldest if full (drop-tail) to keep latest.
  void enqueue(Frame f)
  {
    std::unique_lock<std::mutex> lk(m_);
    if (q_.size() >= cap_)
    {
      q_.erase(q_.begin()); // drop oldest
      drops_++;
    }
    q_.emplace_back(std::move(f));
    lk.unlock();
    cv_.notify_one();
  }

  // Consumer: blocks until data or shutdown
  bool dequeue(Frame &out)
  {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&]
             { return shutdown_ || !q_.empty(); });
    if (shutdown_)
      return false;
    out = std::move(q_.front());
    q_.erase(q_.begin());
    return true;
  }

  void shutdown()
  {
    std::lock_guard<std::mutex> lk(m_);
    shutdown_ = true;
    cv_.notify_all();
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }
  uint64_t drops() const { return drops_.load(); }

private:
  size_t cap_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::vector<Frame> q_;
  std::atomic<uint64_t> drops_{0};
  bool shutdown_{false};
};

// Last-value cache (metadata only). Thread-safe with a small mutex.
class LastValueCache
{
public:
  void publish(const FrameMeta &m)
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto &cur = by_series_[m.series];
    if (m.ts_ns > cur.ts_ns || (m.ts_ns == cur.ts_ns && m.frame_idx > cur.frame_idx))
    {
      cur = m;
    }
  }

  std::optional<FrameMeta> get(const std::string &series) const
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_series_.find(series);
    if (it == by_series_.end())
      return std::nullopt;
    return it->second;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, FrameMeta> by_series_;
};

// Simple append-only segment writer with index.jsonl next to it.
// Visibility contract: write payload, then append index line (no per-frame fsync).
// Roll based on time/size can be added later; here we keep a single segment file for simplicity.
class SegmentWriter
{
public:
  explicit SegmentWriter(const std::filesystem::path &root)
      : root_(root),
        seg_path_(root_ / "00000001.mrd"),
        index_path_(root_ / "index.jsonl")
  {
    std::filesystem::create_directories(root_);
    seg_ = fopen(seg_path_.string().c_str(), "ab");
    index_ = fopen(index_path_.string().c_str(), "ab");
    if (!seg_ || !index_)
      throw std::runtime_error("SegmentWriter open failed");
  }

  ~SegmentWriter()
  {
    if (seg_)
      fclose(seg_);
    if (index_)
      fclose(index_);
  }

  // Append one frame; returns filled meta.path/offset/bytes
  FrameMeta append(const Frame &f)
  {
    FrameMeta out = f.meta;
    // append payload
    long offset = ftell(seg_);
    if (offset < 0)
      throw std::runtime_error("ftell failed");
    size_t nw = fwrite(f.payload.data(), 1, f.payload.size(), seg_);
    if (nw != f.payload.size())
      throw std::runtime_error("segment write failed");
    // visibility: write index line
    out.path = seg_path_.string();
    out.offset = static_cast<uint64_t>(offset);
    out.bytes = static_cast<uint32_t>(f.payload.size());
    write_index_line(out);
    return out;
  }

private:
  void write_index_line(const FrameMeta &m)
  {
    // very small JSON; keep it hand-rolled for speed
    std::string line = "{\"ev\":\"frame\",\"series\":\"" + m.series + "\","
                                                                      "\"frame\":" +
                       std::to_string(m.frame_idx) + ","
                                                     "\"ts_ns\":" +
                       std::to_string(m.ts_ns) + ","
                                                 "\"path\":\"" +
                       escape_json(m.path) + "\","
                                             "\"offset\":" +
                       std::to_string(m.offset) + ","
                                                  "\"bytes\":" +
                       std::to_string(m.bytes) + "}\n";
    fwrite(line.data(), 1, line.size(), index_);
    // intentionally no fflush/fdatasync per frame
  }

  static std::string escape_json(const std::string &s)
  {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s)
    {
      if (c == '\\' || c == '"')
      {
        o.push_back('\\');
        o.push_back(c);
      }
      else
        o.push_back(c);
    }
    return o;
  }

  std::filesystem::path root_;
  std::filesystem::path seg_path_;
  std::filesystem::path index_path_;
  FILE *seg_{nullptr};
  FILE *index_{nullptr};
};
