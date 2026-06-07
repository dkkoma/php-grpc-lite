#!/usr/bin/env bash
#
# Build grpc.so with Clang ThinLTO and collect optimizer remarks / DSO reports.
#
# Usage:
#   tools/diagnostic/optimizer-dso-report.sh [output-dir]
#
set -euo pipefail

cd "$(dirname "$0")/../.."

report_dir="${1:-var/optimizer-dso-report/$(date +%Y%m%d-%H%M%S)}"

docker compose run --rm --no-deps \
    -e REPORT_DIR="/workspace/$report_dir" \
    dev-optimizer bash -lc '
        set -euo pipefail
        cd /workspace

        out="$REPORT_DIR"
        rm -rf "$out"
        mkdir -p "$out/remarks"

        make clean >/tmp/grpc-optimizer-clean.log 2>&1 || true
        rm -rf .libs modules *.lo *.o *.dep
        find . -maxdepth 3 \( -name "*.opt.yaml" -o -name "*.opt.ld.yaml*" \) -delete

        phpize >"$out/phpize.log" 2>&1
        CC="clang -fuse-ld=lld" \
        CFLAGS="-g -gdwarf-4 -O2 -flto=thin" \
        LDFLAGS="-flto=thin -fuse-ld=lld" \
            ./configure --enable-grpc >"$out/configure.log" 2>"$out/configure.err"
        objects="grpc.lo src/protocol_core.lo src/status_core.lo src/transport_core.lo src/tls_config.lo src/surface.lo src/transport.lo src/unary_call.lo src/server_streaming_call.lo src/wrapper_adapter.lo"
        make -j"$(nproc)" $objects \
            CFLAGS_CLEAN="-g -gdwarf-4 -O2 -flto=thin -Rpass=inline -Rpass-missed=inline -fsave-optimization-record -D_GNU_SOURCE" \
            >"$out/make.log" 2>"$out/make.err"
        make -j"$(nproc)" \
            CFLAGS_CLEAN="-g -gdwarf-4 -O2 -flto=thin -D_GNU_SOURCE" \
            >>"$out/make.log" 2>>"$out/make.err"

        cp modules/grpc.so "$out/grpc.so"
        php -d extension="$out/grpc.so" -r "exit(extension_loaded(\"grpc\") ? 0 : 1);" >"$out/load.log" 2>&1

        {
            clang --version | head -n 1
            ld.lld --version | head -n 1 || true
            bloaty --version | head -n 1
        } >"$out/tool-versions.txt"

        file "$out/grpc.so" >"$out/file.txt"
        size -A "$out/grpc.so" >"$out/size-A.txt"
        readelf -Ws "$out/grpc.so" >"$out/readelf-symbols.txt"
        readelf -Wr "$out/grpc.so" >"$out/readelf-relocations.txt"
        nm -S --size-sort "$out/grpc.so" >"$out/nm-size-sort.txt"
        objdump -dr "$out/grpc.so" >"$out/objdump-dr.txt"
        bloaty "$out/grpc.so" -d sections >"$out/bloaty-sections.txt"
        bloaty "$out/grpc.so" -d compileunits >"$out/bloaty-compileunits.txt"
        bloaty "$out/grpc.so" -d symbols >"$out/bloaty-symbols.txt"

        find . -maxdepth 3 \( -name "*.opt.yaml" -o -name "*.opt.ld.yaml*" \) -exec cp --parents {} "$out/remarks" \;

        {
            printf "report_dir=%s\n" "$out"
            cat "$out/tool-versions.txt"
            grep -c "remark: .* inlined" "$out/make.err" | awk "{print \"clang_inline_success=\" \$1}"
            grep -c "remark: .* not inlined" "$out/make.err" | awk "{print \"clang_inline_missed=\" \$1}"
            stat -c "grpc_so_bytes=%s" "$out/grpc.so"
        } >"$out/summary.txt"
    '

printf "optimizer DSO report: %s\n" "$report_dir"
