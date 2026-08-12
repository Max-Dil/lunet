#!/bin/bash

mkdir -p build && cd build
cmake clean
cmake ..
make