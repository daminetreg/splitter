#include "bench_common.hpp"

polygon_t make_square(double x, double y, double side)
{
    polygon_t p;
    bg::append(p, point_t(x, y));
    bg::append(p, point_t(x, y + side));
    bg::append(p, point_t(x + side, y + side));
    bg::append(p, point_t(x + side, y));
    bg::append(p, point_t(x, y));
    bg::correct(p);
    return p;
}

static double perimeter_of(polygon_t const& p)
{
    return bg::perimeter(p);
}

static double centroid_x(polygon_t const& p)
{
    point_t c;
    bg::centroid(p, c);
    return bg::get<0>(c);
}

double measure_area()
{
    polygon_t const a = make_square(0.0, 0.0, 2.0);
    polygon_t const b = make_square(1.0, 1.0, 4.0);

    multi_polygon_t mp;
    mp.push_back(a);
    mp.push_back(b);

    double total = bg::area(a) + bg::area(b) + bg::area(mp);
    total += perimeter_of(a) + perimeter_of(b);
    total += centroid_x(a) + centroid_x(b);

    return bench_weight(total);
}
