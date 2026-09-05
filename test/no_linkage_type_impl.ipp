// A type in an unnamed namespace, and functions whose signatures mention it.
//
// Such a type has no linkage of its own, so a function taking it can only be defined in the
// translation unit that declares the type. Splitting the definition into a piece of its own
// produces
//
//     error: function 'entry_start' is used but not defined in this translation unit, and
//            cannot be defined in any other translation unit because its type does not have
//            linkage
//
// Boost.Test writes exactly this in unit_test_log.ipp, and every Boost.Geometry test includes
// the framework in header-only mode, so every one of them fell back on it.
//
// The function's own linkage says nothing: libclang reports `entry_start` as external,
// because it is the *type* that is unique to this unit.
namespace { struct helper_impl { int v; }; }

bool entry_start( helper_impl& h )
{
    return h.v > 0;
}

// Through a template argument rather than directly: `holder<helper_impl>` has external
// linkage of its own while `helper_impl` does not, so the check has to look inside.
int count_started( holder<helper_impl> const& hs )
{
    int n = 0;
    for ( int i = 0; i < hs.size; ++i ) {
        helper_impl h = hs.items[i];
        if ( entry_start( h ) ) ++n;
    }
    return n;
}

int run_helper()
{
    holder<helper_impl> hs;
    hs.items[0] = helper_impl{ 4 };
    hs.items[1] = helper_impl{ 0 };
    hs.size = 2;
    return entry_start( hs.items[0] ) ? hs.items[0].v + count_started( hs ) : 0;
}
