#!/usr/bin/env bash

if [[ $1 = clean ]]; then
  printf -- "-- cleaning ...\n"
  rm -rf build
else
  [[ $1 = afresh ]] && $0 clean

  printf -- "-- building ...\n"
  [[ ! -f build.env ]] && \
    printf -- '-- error: run `cp build.env.example build.env` and edit build.env with valid config' >&2 && \
    exit 1
  mkdir -p build && source build.env && cmake -B build && cd build && make -j$(nproc); cd ..
fi

