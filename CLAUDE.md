Be super succinct and synthetic in your explanations.

# Work ethic
- Before any work add a TODO/ as .md file with the following section : Motivation, Implementation Proposal, Acceptance Criteria. 
  - Always plan unit test for the cpp-splitter unit test suite with CTest and an example/boost-to-split build to confirm (first just filesystem and then spirit with our integration test harness with example/spirit-tests/SpiritTestsFromJamfiles.cmake.

- Once work is done, commit following the emojcode to prefix commits : 
  * :barber: `:barber:` CLEANUP: removing old thing, unuseful code, badly written variable name...
  * :lipstick: `:lipstick:`  EDITORIAL: changes which beautify the codebase or the app
  * :rocket: `:rocket:` RELEASE commit
  * :book: `:book:` DOC
  * :new: `:new:` FEATURE
  * :wrench: `:wrench:` BUGFIX
  * :recycle: `:recycle:` REFACTORING
  * :gear: `:gear:` CONFIG
  * :mag: `:mag:` TEST: unit tests changed / added

# If you are not in a tipi container or a devcontainer, on linux to compile the cpp-splitter

1. Create docker container if it doesn't exists

```sh
mkdir -p ../`whoami`-tipi-workdir-vT.w
mkdir -p ../generalized-toolchains
docker run --init --detach --name `whoami`-tipi  -u`id -u`:`id -g` --group-add tipi -e TIPI_CACHE_CONSUME_ONLY=ON -e TIPI_CACHE_FORCE_ENABLE=OFF -e HOME -v $HOME:$HOME:rw \
  --mount type=bind,source=$PWD/../`whoami`-tipi-workdir-vT.w,target=/usr/local/share/.tipi/vT.w/ \
  --mount type=bind,source=$PWD/../generalized-toolchains,target=/usr/local/share/.tipi/environments/generalized/v1/ \
  -v $PWD:$PWD:rw -w $PWD \
  tipibuild/tipi-ubuntu-2404:v0.0.82 \
  sleep infinity

docker exec -u 0 `whoami`-tipi useradd -d $HOME -u `id -u` `whoami`
```

2. Otherwise if it exists enter the interactive shell
```
# This launches a container interactive shell
docker exec -it `whoami`-tipi tipi run /bin/bash
```

Then run the compilation commands below.

# If you are in a tipi container or a devcontainer 
You can just use the compilation commands bellow.

# Compilation Commands
3. Configure the splitter with `cmake -GNinja -S . -B build/ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake`
4. Build the splitter with `cmake --build build/ -j32`


# When running benchmarks with `cmake-re --host --distributed`

cmake-re outputs :
```
Invocation ID: 4e136242-bb77-482a-b563-37b619a446d4
Proxy started successfully.
Remote execution proxy started sucessfully
```

You can download after the build the EngFlow profile with cURL using the same mTLS key than for the build to authenticate on : https://${RBE_service}/api/profiling/v1/instances/default/invocations/4e136242-bb77-482a-b563-37b619a446d4

The download will land a chrome tracing file which can be analyzed with perfetto.