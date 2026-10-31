#!/usr/bin/env sh
set -e
# one flag set for every binary so the benchmark measures the same code the kernel runs
FLAGS="-std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude"
clang++ $FLAGS -o build/kernel.exe src/main.cpp src/kernel.cpp
clang++ $FLAGS -o build/test_kernel.exe tests/test_kernel.cpp src/kernel.cpp
clang++ $FLAGS -o build/bench.exe bench/bench.cpp src/kernel.cpp
