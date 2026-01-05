// Thread-safe wrapper for CircularBuffer
// This is a LOCAL ADDITION (not from upstream/robot-data-marshal)
// Wraps the upstream CircularBuffer to add thread-safety

#ifndef THREADSAFE_CIRCULAR_BUFFER_HPP
#define THREADSAFE_CIRCULAR_BUFFER_HPP

#include "circularBuffer.hpp"
#include <shared_mutex>

template<typename T>
class ThreadSafeCircularBuffer {
public:
    explicit ThreadSafeCircularBuffer(size_t capacity)
        : buffer_(capacity) {}

    void push(const T& item) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        buffer_.push(item);
    }

    bool pop(T& item) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return buffer_.pop(item);
    }

    bool peek(T& item, int k = 1) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return const_cast<CircularBuffer<T>&>(buffer_).peek(item, k);
    }

    bool empty() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return buffer_.empty();
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return buffer_.size();
    }

    bool is_full() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return buffer_.is_full();
    }

private:
    CircularBuffer<T> buffer_;
    mutable std::shared_mutex mutex_;
};

#endif // THREADSAFE_CIRCULAR_BUFFER_HPP
