#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
check_cpp="$root/tests/watch_all_task4_offline_check.cpp"
bin="${TMPDIR:-/tmp}/ue4dumper_watch_all_task4_check"

c++ -std=c++14 -Wall -Wextra -Werror -pthread -I"$root/jni" "$check_cpp" -o "$bin"
"$bin"
