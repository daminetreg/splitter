// Shared header for the Boost.Geometry incremental benchmark.
//
// Everything expensive lives here: the Geometry algorithm headers whose tag dispatch over the
// concept hierarchy is what makes one of these translation units cost what it costs. Every
// unit includes it, so an edit to it is the case a splitter has to earn its keep on.
//
// Deliberately no Boost.Test. Its header-only mode is unsplittable under the current design
// (TODO/24): progress_monitor.ipp defines a macro, uses it in four out-of-line member
// functions and undefines it sixty lines later, so those definitions can go neither to the
// definitions header nor stay duplicated in the preamble. A benchmark that included it would
// be timing fallbacks rather than splits, which is the mistake the Spirit benchmark made
// once already.
//
// `bench_weight()` is the body-edit target: an ordinary inline function in a widely included
// header, the shape whose body a developer changes twenty times an hour.
#pragma once

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/multi/geometries/multi_polygon.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace bg = boost::geometry;

typedef bg::model::d2::point_xy<double>   point_t;
typedef bg::model::polygon<point_t>       polygon_t;
typedef bg::model::multi_polygon<polygon_t> multi_polygon_t;
typedef bg::model::linestring<point_t>    linestring_t;
typedef bg::model::box<point_t>           box_t;

// Body-edit target: an inline function in a widely included header.
inline double bench_weight(double v) {
    return v * 1.0;
}

// Each unit contributes one number, so the driver can check that the split program computes
// what the plain one does rather than only that it links.
double measure_area();
double measure_distance();
double measure_overlay();
double measure_hull();

polygon_t make_square(double x, double y, double side);
