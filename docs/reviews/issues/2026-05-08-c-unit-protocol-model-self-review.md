# C unit protocol model self review 2026-05-08

## Scope

- `ext/grpc/protocol_core.c`
- `ext/grpc/status_core.c`
- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/unit/*.c`
- `ext/grpc/tests/*.phpt`
- `bench/phase2/check-native-c-unit.sh`
- `bench/phase2/check-native-phpt-coverage.sh`
- `README.md`
- `docs/code-reading-guide.md`

## Reviewer Role

- protocol model reviewer

## Review Prompt Summary

- gRPC / HTTP/2 domain concepts、status taxonomy、protocol helper責務、transport I/Oとの境界、test/production boundary、C unitとPHPTの責務分離が現在の実装で自然にモデル化されているかを確認した。

## Issues

### REVIEW-20260508-CUNIT-PROTOCOL-001: C unitはprivate `grpc_call` modelを直接参照する

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `protocol model reviewer`
- Finding: `status_core` のC unitは `grpc_call` 相当の軽量test doubleを別定義せず、`internal.h` のprivate `grpc_call` を直接使ってstatus taxonomyを検証している。
- Evidence: `ext/grpc/tests/unit/test_status_core.c`, `ext/grpc/internal.h`, `ext/grpc/status_core.c`
- Expected model: gRPC call lifecycle / status taxonomyの入力状態はproductionの `grpc_call` が単一のdomain modelとして所有し、test側で別モデルを作って乖離させない。
- Why it matters: test doubleで `grpc_call` の一部を再定義すると、field名やpriority orderがproduction modelから乖離してもunitがgreenになりうる。private modelを直接使う方が、status taxonomyのownerを明確に保てる。
- Recommended fix: 現状維持。C unit compile時のPHP header由来unused warningはrunner側で限定的に抑制し、production headerやCFLAGSへ漏らさない。
- Fix summary: `check-native-c-unit.sh` と `check-native-phpt-coverage.sh` で `-Wno-unused-function -Wno-unused-variable` をC unit compileに限定して指定した。
- Fix commit: `this commit`
- Verification: `./bench/phase2/check-native-c-unit.sh`; `./bench/phase2/check-native-phpt.sh`; `./bench/phase2/check-native-phpt-coverage.sh`; `./bench/phase2/check-native-static-analysis.sh`; `docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit`
- Notes: `protocol_core.c` はPHP runtimeに依存しないpure helper、`status_core.c` は `grpc_call` flags / HTTP status / HTTP/2 stream errorからfinal gRPC status codeを決めるtaxonomy ownerとして分離されている。transport I/O、TLS、nghttp2 event loop、PHP object lifecycleはC unit対象から除外されている。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1`
