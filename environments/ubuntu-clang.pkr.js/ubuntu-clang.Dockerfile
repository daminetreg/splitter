# The environment CMake RE builds in.
#
# Inherits the exact image the rest of this repository builds in -- the devcontainer, the
# `docker run` recipe in CLAUDE.md and .github/workflows/build-and-test.yml all name it -- so
# that a cmake-re build and a local build are the same build. The clang that
# environments/ubuntu-clang.cmake points at, and the libc++ the link line needs, both live in
# this image at a fixed path.
#
# Nothing is added on top. The base already carries the toolchain, CMake, ninja and git, and
# duplicating any of that here would only create a second thing to keep in step with it.
FROM tipibuild/tipi-ubuntu-2404:v0.0.87
