#pragma once
#include <vector>
// We use a template so the buffer can hold any type: int, float, struct, etc.

template<typename T>
class CircularBuffer {
public:

    // Constructor: we pass the capacity (maximum number of elements)
    explicit CircularBuffer(size_t capacity)
        : buffer(capacity),  // allocate 'capacity' slots
          head(0),           // head points to the index where the next push will write
          tail(0),           // tail points to the index of the oldest element
          full(false)        // starts empty
    {}

    // Push a new element into the buffer.
    // If the buffer is full, the oldest element will be overwritten.
    void push(const T& item) {
        buffer[head] = item;                   // write the new element at the head position

        if (full) {                            // if full, advancing head would overwrite tail
            tail = (tail + 1) % buffer.size(); // so move tail forward (overwrite oldest)
        }

        head = (head + 1) % buffer.size();     // advance head circularly

        // If head catches up to tail, buffer becomes full
        full = (head == tail);
    }

    // Remove and return the oldest element.
    // Returns false if the buffer is empty.
    bool pop(T &item) {
        if (empty()) return false;             // cannot pop from empty buffer

        item = buffer[tail];                   // get the oldest item

        full = false;                          // if we pop, buffer cannot be full anymore
        tail = (tail + 1) % buffer.size();     // move tail forward circularly

        return true;
    }
    // Return the k newest element.
    // Returns false if the buffer is empty.
    bool peek(T &item, int k) {
        if (empty()) return false;             // cannot peek from empty buffer

        int newest = ((head-(k-1)) + buffer.size() - 1) % buffer.size();
        item = buffer[newest];

        return true;
    }

    // Buffer is empty if not full AND head == tail
    bool empty() const {
        return (!full && (head == tail));
    }

    // Buffer is full if full == true
    bool is_full() const {
        return full;
    }

    // Compute number of stored elements
    size_t size() const {
        if (full) return buffer.size();        // if full, size = capacity

        if (head >= tail)                     // normal case: no wrap-around
            return head - tail;

        // wrapped case: head passed from end to beginning
        return buffer.size() - tail + head;
    }

private:
    std::vector<T> buffer;  // underlying storage array
    size_t head;            // index to write next element
    size_t tail;            // index of the oldest element to read next
    bool full;              // separate state variable to disambiguate head == tail
};
