# The environment CMake RE builds in.
#
# Inherits the exact image the rest of this repository builds in -- the devcontainer, the
# `docker run` recipe in CLAUDE.md and .github/workflows/cmake-re.yml all name
# tipibuild/tipi-ubuntu-2404:v0.0.87 -- so that a cmake-re build and a local build are the same
# build. The clang that environments/ubuntu-clang.cmake points at, and the libc++ the link line
# needs, both live in this image at a fixed path.
#
# Pinned by manifest digest rather than by tag, and that is what makes a distributed build
# possible from a machine with no docker. Given a tag, cmake-re has to resolve it to a digest
# before it can tell the RBE workers which image to run in, and resolving it is what it shells
# out to docker for. Given the digest outright there is nothing to resolve. The digest below is
# v0.0.87's, read from the registry:
#
#   docker-content-digest: sha256:7e1cdb0e91b180172bf481b5b5356de4bc911331e0e074099eeed1481195977b
#
# Nothing is added on top. The base already carries the toolchain, CMake, ninja and git, and
# duplicating any of that here would only create a second thing to keep in step with it.
FROM tipibuild/tipi-ubuntu-2404@sha256:7e1cdb0e91b180172bf481b5b5356de4bc911331e0e074099eeed1481195977b
