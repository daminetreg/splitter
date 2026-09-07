#include "bench_common.hpp"

static double hull_area()
{
    bg::model::multi_point<point_t> mp;
    mp.push_back(point_t(0.0, 0.0));
    mp.push_back(point_t(4.0, 0.0));
    mp.push_back(point_t(4.0, 4.0));
    mp.push_back(point_t(0.0, 4.0));
    mp.push_back(point_t(2.0, 2.0));

    polygon_t hull;
    bg::convex_hull(mp, hull);
    return bg::area(hull);
}

static double simplified_length()
{
    linestring_t ls;
    bg::append(ls, point_t(0.0, 0.0));
    bg::append(ls, point_t(1.0, 0.1));
    bg::append(ls, point_t(2.0, -0.1));
    bg::append(ls, point_t(3.0, 5.0));

    linestring_t out;
    bg::simplify(ls, out, 0.5);
    return bg::length(out);
}

static double within_count()
{
    polygon_t const p = make_square(0.0, 0.0, 4.0);
    int n = 0;
    n += bg::within(point_t(1.0, 1.0), p) ? 1 : 0;
    n += bg::within(point_t(9.0, 9.0), p) ? 1 : 0;
    n += bg::covered_by(point_t(2.0, 2.0), p) ? 1 : 0;
    return static_cast<double>(n);
}

double measure_hull()
{
    return bench_weight(hull_area() + simplified_length() + within_count());
}
