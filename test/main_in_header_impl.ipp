// `main` defined in an implementation include, as Boost.Test's unit_test_main.ipp does.
//
// A split piece taken out of a header is written `inline`, so that several objects may carry
// the same definition and the linker merge them. `main` may not be inline:
//
//     unit_test_main.ipp:292:1: error: 'main' is not allowed to be declared inline
//
// Every Boost.Geometry test includes the Boost.Test framework in header-only mode, so every
// one of them hit it. There is nothing to gain from splitting it either: a program has one
// main and it can only be emitted once.
int main()
{
    return probe() + helper() + unit_contribution() == 15 ? 0 : 1;
}
