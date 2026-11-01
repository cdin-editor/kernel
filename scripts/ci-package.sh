#!/usr/bin/env bash
set -euo pipefail

: "${PIPELINE_RELEASE_TAG:?Pipeline must provide PIPELINE_RELEASE_TAG}"
package_version="${PIPELINE_RELEASE_TAG#v}"
bash scripts/install-deps.sh --prefix .deps
make clean
make all shared ffi-shared quickstart
stage="$PWD/build/stage"
rm -rf "$stage" dist
make PACKAGE_VERSION="$package_version" DESTDIR="$stage" PREFIX=/cdin install
EXPECTED_PACKAGE_VERSION="$package_version" CC="${CC:-cc}" bash scripts/verify-installed.sh "$stage/cdin"
PACKAGE_VERSION="$package_version" STAGED_PREFIX="$stage/cdin" bash scripts/package-release.sh
archive="$(find dist -maxdepth 1 -type f \( -name '*.tar.gz' -o -name '*.zip' \) -print -quit)"
test -n "$archive"
CC="${CC:-cc}" bash scripts/test-release-package.sh "$archive" "$package_version"
