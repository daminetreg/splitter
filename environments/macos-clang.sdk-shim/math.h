/* Lets tipi's clang 13 read the macOS SDK's math.h.
 *
 * From the macOS 15 SDK on, math.h declares half-precision helpers -- __fabsf16(_Float16) and
 * a dozen siblings -- with no guard, on the assumption that the compiler is Apple's, which has
 * had _Float16 on x86_64 since clang 15. Clang 13 has not, and rejects the declarations as
 * "_Float16 is not supported on this target". Nothing in libc++ or in this project calls them,
 * so for the length of that one header the type is spelled `float` instead. The declarations
 * are wrong for a caller, but there is none, and the SDK ships no definitions to mismatch.
 *
 * Found first because environments/macos-clang.cmake puts this directory on -isystem ahead of
 * libc++'s own math.h wrapper; #include_next then goes on to the wrapper and, through it, to
 * the SDK.
 */
#pragma once
#define _Float16 float
#include_next <math.h>
#undef _Float16
