# TLS config extraction domain review 2026-05-30

## Scope

- `src/tls_config.c`
- `src/tls_config.h`
- `src/transport.c`
- `src/transport.h`
- `config.m4`
- `tools/test/check-c-static-analysis.sh`
- `docs/issues/open/2026-05-29-tls-implement-judge.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- TLS setup extractionについて、transport vs TLS setup責務、h2 connection lifecycle、TLS/mTLS behavior、error semantics、public/internal boundaries、production vs diagnostic boundaries、source-list/build consistencyを確認した。

## Issues

none

## Review Notes

- `src/tls_config.c` は root PEM store設定、client certificate/private key設定、SNI / hostname verification name設定に限定されており、HTTP/2 connection lifecycle、`SSL_connect` loop、deadline poll、`SSL_read` / `SSL_write`、ALPN `h2` 設定と確認は `src/transport.c` に残っている。
- `h2_connection` は引き続き fd / `SSL_CTX` / `SSL` / nghttp2 session / connection error detailを所有しており、TLS setup moduleは connection cache、stream table、gRPC status、PHP object surfaceを所有していない。
- TLS/mTLS behaviorは抽出前の分岐を維持している。root certsがある場合は provided roots、ない場合は default verify paths、client cert/keyは両方が揃う場合だけ設定し、片方だけの指定は transport側で connection setup errorとして扱う。
- TLS setup failureは引き続き connection setup failureとして `last_error_detail` に写され、gRPC application statusやstream-local failureへ畳まれていない。
- `src/tls_config.h` は `src/common.h` 配下のprivate C extension headerであり、PHP public APIやbench/diagnostic surfaceへ内部OpenSSL setup概念を露出していない。
- `config.m4` と `tools/test/check-c-static-analysis.sh` の production source listに `src/tls_config.c` が追加され、bench-only sourcesは従来どおり `PHP_GRPC_LITE_ENABLE_BENCH` 側に限定されている。
- `docs/issues/open/2026-05-29-tls-implement-judge.md` は、nghttp2へTLS stackを移さず、certificate / peer identity setupだけを内部moduleへ抽出する現在の判断を記録している。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
