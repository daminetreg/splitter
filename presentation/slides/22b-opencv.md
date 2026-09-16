---
chapter: Benchmarks
chapter-label: OpenCV core and imgproc
notes: What is being built. OpenCV's core module is the Mat class and everything around it -- matrices, sparse matrices, arithmetic, allocation -- and imgproc the image operations on top. Ordinary library code: the classes are declared in mat.hpp, their small members are defined inline in mat.inl.hpp, which every unit includes, the large ones out of line in the .cpp files. 158 units, static libraries, nothing optional. On the right, the kind of code a user of the library writes; on the left, the shape of the library itself.
---
## OpenCV: {violet}158 units{/violet} of ordinary C++,  
one inline header every unit includes.

::: code-columns
```cpp
// modules/core/include/opencv2/core/mat.hpp
class CV_EXPORTS SparseMat
{
public:
    SparseMat();
    SparseMat(int dims, const int* sizes, int type);
    //! returns the number of non-zero elements
    size_t nzcount() const;
    //! computes the element hash value
    size_t hash(int i0) const;
    template<typename _Tp> _Tp& ref(int i0, size_t* hashval = 0);
    // ...
    Hdr* hdr;
};

// modules/core/src/matrix_sparse.cpp
SparseMat::SparseMat(int _dims, const int* _sizes, int _type)
    : flags(MAGIC_VAL), hdr(0)
{
    create(_dims, _sizes, _type);
}
```
---
```cpp
// what a user writes
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

cv::Mat edges_of(const cv::Mat& image)
{
    cv::Mat gray, blurred, edges;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.4);
    cv::Canny(blurred, edges, 50, 150);
    return edges;
}
```
:::

::: tiny
OpenCV 4.11.0, `core` and `imgproc`, static, `-DBUILD_LIST=core,imgproc`, nothing optional · 158 C++ units · clang 13, C++17, Release · every unit splits: 0 fallbacks, 0 declined
:::
