#!/bin/sh
set -eu

results_dir="${RESULTS_DIR:-/results}"
bench_script="${BENCH_SCRIPT:-select-table-marked-app-extra-header.php}"
iter="${ITER:-20}"
tcpdump_enabled="${TCPDUMP:-1}"
tcpdump_filter="${TCPDUMP_FILTER:-tcp port 443}"
tcpdump_interface="${TCPDUMP_INTERFACE:-any}"
tcpdump_stop_grace="${TCPDUMP_STOP_GRACE:-1}"
php_ini_args="${PHP_INI_ARGS:-}"

mkdir -p "$results_dir"
rm -f "$results_dir/markers.log" "$results_dir/php.err" "$results_dir/trace.jsonl" "$results_dir/tcpdump.pcap" "$results_dir/summary.txt"

if [ "$tcpdump_enabled" = "1" ]; then
  tcpdump -Z root -U -i "$tcpdump_interface" -nn -s 0 -w "$results_dir/tcpdump.pcap" $tcpdump_filter >"$results_dir/tcpdump.log" 2>&1 &
  tcpdump_pid=$!
  sleep 0.2
else
  tcpdump_pid=""
fi

set +e
GRPC_LITE_TRACE_FILE="$results_dir/trace.jsonl" \
  GRPC_OFFICIAL_FRAME_TRACE_FILE="$results_dir/trace.jsonl" \
  php $php_ini_args "/app/$bench_script" "$iter" >"$results_dir/markers.log" 2>"$results_dir/php.err"
rc=$?
set -e

if [ -n "$tcpdump_pid" ]; then
  sleep "$tcpdump_stop_grace"
  kill -INT "$tcpdump_pid" >/dev/null 2>&1 || true
  wait "$tcpdump_pid" >/dev/null 2>&1 || true
  tcpdump -tttt -nn -r "$results_dir/tcpdump.pcap" >"$results_dir/tcpdump.txt" 2>"$results_dir/tcpdump-read.err" || true
fi

php /app/wire-summary.php "$results_dir/trace.jsonl" "$results_dir/markers.log" >"$results_dir/summary.txt" 2>"$results_dir/summary.err" || true
cat "$results_dir/summary.txt"
exit "$rc"
