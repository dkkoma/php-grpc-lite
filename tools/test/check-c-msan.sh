#!/usr/bin/env bash
#
# Run the native extension pure C core unit suite under Clang MemorySanitizer.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

SANITIZER_KIND=memory ./tools/test/check-c-sanitizer.sh
