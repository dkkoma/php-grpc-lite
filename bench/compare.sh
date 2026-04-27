#!/usr/bin/env bash
#
# Compatibility wrapper for the regular comparison. The implementation lives in
# bench/run.sh so log parsing and artifact names stay consistent.
#
# Usage:  ./bench/compare.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."

exec ./bench/run.sh compare
