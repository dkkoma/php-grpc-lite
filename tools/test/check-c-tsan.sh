#!/usr/bin/env bash
#
# Run the native extension C unit and PHPT suite under Clang ThreadSanitizer.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

SANITIZER_KIND=thread ./tools/test/check-c-sanitizer.sh
