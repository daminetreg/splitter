Be super succinct and synthetic in your explanations.

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