#!/usr/bin/env bash
set -euo pipefail

bash scripts/install-deps.sh --prefix .deps
make clean
make all shared ffi-shared quickstart
make check
bash scripts/check-public-header.sh
