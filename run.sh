#!/bin/bash

set -euo pipefail

meson compile -C build
./build/jade "$@"
