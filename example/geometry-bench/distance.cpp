#include "bench_common.hpp"

static double segment_distance()
{
    linestring_t ls;
    bg::append(ls, point_t(0.0, 0.0));
    bg::append(ls, point_t(3.0, 4.0));
    bg::append(ls, point_t(6.0, 0.0));
    return bg::distance(point_t(0.0, 5.0), ls);
}

static double box_distance()
{
    box_t const b(point_t(0.0, 0.0), point_t(1.0, 1.0));
    return bg::distance(point_t(4.0, 5.0), b)
         + bg::comparable_distance(point_t(4.0, 5.0), b);
}

double measure_distance()
{
    polygon_t const p = make_square(0.0, 0.0, 2.0);
    double total = bg::distance(point_t(5.0, 5.0), p);
    total += segment_distance();
    total += box_distance();
    return bench_weight(total);
}
