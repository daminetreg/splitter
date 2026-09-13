// Regression fixture for TODO/42 (4): a special member defaulted out of line,
// `T::~T() = default;`, as OpenCV's cuda_gpu_mat_nd.cpp writes GpuMatND's destructor.
// libclang's extent for the definition ends at the declarator; cutting that out of the
// preamble left ` = default;` behind, and the piece carried a declarator with no definition.
#include <cstdio>
#include <vector>

struct Buffer {
    Buffer();
    ~Buffer();
    Buffer(const Buffer&) = delete;          // in-class: nothing to move
    Buffer& operator=(Buffer&&);
    int total() const;
    std::vector<int> data;
};

Buffer::Buffer() : data{1, 2, 3} {}
Buffer::~Buffer() = default;
Buffer& Buffer::operator=(Buffer&&) = default;
int Buffer::total() const { int s = 0; for (int v : data) s += v; return s; }

int main()
{
    Buffer b;
    Buffer c;
    c = static_cast<Buffer&&>(b);
    if (c.total() != 6) return 1;
    std::printf("%d\n", c.total());
    return 0;
}
