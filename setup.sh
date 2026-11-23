#!/bin/bash

set -euo pipefail

meson setup build --native-file llvm.ini

