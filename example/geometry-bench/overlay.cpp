#include "bench_common.hpp"

static double intersection_area()
{
    polygon_t const a = make_square(0.0, 0.0, 3.0);
    polygon_t const b = make_square(1.0, 1.0, 3.0);
    multi_polygon_t out;
    bg::intersection(a, b, out);
    return bg::area(out);
}

static double union_area()
{
    polygon_t const a = make_square(0.0, 0.0, 3.0);
    polygon_t const b = make_square(1.0, 1.0, 3.0);
    multi_polygon_t out;
    bg::union_(a, b, out);
    return bg::area(out);
}

static double difference_area()
{
    polygon_t const a = make_square(0.0, 0.0, 3.0);
    polygon_t const b = make_square(1.0, 1.0, 3.0);
    multi_polygon_t out;
    bg::difference(a, b, out);
    return bg::area(out);
}

double measure_overlay()
{
    return bench_weight(intersection_area() + union_area() + difference_area());
}
