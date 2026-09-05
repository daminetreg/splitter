SINGLETON_CONS_IMPL(collector)

// An ordinary out-of-line member after the invocation. It is what gets corrupted when the
// invocation is left unterminated, and what proves the file is still whole when it is not.
collector&
collector::mark( int d )
{
    v += d;
    return *this;
}
