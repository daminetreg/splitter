#pragma once
// A class defined in an unnamed namespace by a header, as OpenCV's
// allocator_stats.impl.hpp defines AllocatorStatistics. Every translation unit that includes
// this gets its own type; a variable of it cannot be named from any other.
namespace {
class Counter {
public:
    void note(int v) { total_ += v; ++count_; }
    int total() const { return total_; }
    int count() const { return count_; }
private:
    int total_ = 0;
    int count_ = 0;
};
}  // namespace
