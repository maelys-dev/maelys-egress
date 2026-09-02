#!/bin/sh
set -eu

image=maelys-egress-test
docker build -f docker/Dockerfile.test -t "$image" .
docker run --rm "$image" make clean check CC=clang CXX=clang++
docker run --rm "$image" make clean check CC=gcc CXX=g++
docker run --rm "$image" make asan-ubsan
docker run --rm "$image" make tsan
