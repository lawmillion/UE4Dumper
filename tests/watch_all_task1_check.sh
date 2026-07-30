#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

process_h="$root/jni/Process.h"
mem_h="$root/jni/Mem.h"
kmods_cpp="$root/jni/kmods.cpp"
kmods_h="$root/jni/kmods.h"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local description="$3"
    if ! grep -Eq -- "$pattern" "$file"; then
        echo "FAIL: missing ${description} in ${file}" >&2
        exit 1
    fi
}

require_pattern "$process_h" 'bool[[:space:]]+TryReadBuffer[[:space:]]*\(' 'strict TryReadBuffer API'
require_pattern "$process_h" 'std::vector<char>[[:space:]]+tmp' 'TryReadBuffer temporary buffer'
require_pattern "$process_h" 'memcpy\(buffer,[[:space:]]*tmp\.data\(\)' 'TryReadBuffer commits only after complete read'
require_pattern "$mem_h" 'bool[[:space:]]+TryRead[[:space:]]*\(' 'strict TryRead<T> API'
require_pattern "$mem_h" 'T[[:space:]]+tmp' 'TryRead<T> temporary value'
require_pattern "$kmods_cpp" 'watch-all' '--watch-all long option'
require_pattern "$kmods_cpp" 'interval' '--interval long option'
require_pattern "$kmods_cpp" 'RunWatchAll[[:space:]]*\(' 'watch mode entry call'
require_pattern "$kmods_h" 'PidAlive[[:space:]]*\(' 'PID liveness helper'
require_pattern "$kmods_cpp" 'SIGINT' 'SIGINT stop handling'
require_pattern "$kmods_cpp" 'isWatchAll' 'independent watch mode flag'

# Legacy dump flags must remain present.
for opt in strings objs sdku sdkw; do
    require_pattern "$kmods_cpp" "${opt}" "legacy --${opt} option"
done

echo "watch-all task 1 checks passed"
