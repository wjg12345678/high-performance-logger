#!/usr/bin/env bash
set -euo pipefail

script_dir=$(
  CDPATH= cd -- "$(dirname -- "$0")"
  pwd
)
repo_root=$(dirname "$script_dir")

prefix_dir=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hlog-install-XXXXXX")}
build_dir=${2:-$(mktemp -d "${TMPDIR:-/tmp}/hlog-build-XXXXXX")}
consumer_build_dir=${3:-$(mktemp -d "${TMPDIR:-/tmp}/hlog-consumer-build-XXXXXX")}
cleanup_prefix=${1:-}
cleanup_build=${2:-}
cleanup_consumer=${3:-}

cleanup() {
  if [[ -z "$cleanup_prefix" ]]; then
    rm -rf "$prefix_dir"
  fi
  if [[ -z "$cleanup_build" ]]; then
    rm -rf "$build_dir"
  fi
  if [[ -z "$cleanup_consumer" ]]; then
    rm -rf "$consumer_build_dir"
  fi
}
trap cleanup EXIT

echo "install prefix: $prefix_dir"
echo "library build dir: $build_dir"
echo "consumer build dir: $consumer_build_dir"

cmake -S "$repo_root" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOG_BUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel
cmake --install "$build_dir" --prefix "$prefix_dir"

cmake -S "$repo_root/examples/find_package_consumer" -B "$consumer_build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix_dir"
cmake --build "$consumer_build_dir" --parallel
"$consumer_build_dir/hlog_find_package_consumer"
