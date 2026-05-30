# Phase 2 core helper boundary domain review 2026-05-30

## Scope

- `config.m4`
- `main.c`
- `internal.h`
- `protocol_core.c`
- `protocol_core.h`
- `status_core.c`
- `status_core.h`
- `transport.c`
- `transport_core.c`
- `transport_core.h`
- `tests/unit/test_protocol_core.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- `tests/fuzz/fuzz_protocol_core.c`
- `tools/test/check-c-unit.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-c-fuzz.sh`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Phase 2の未コミット変更について、`protocol_core` / `status_core` / `transport_core` が `.c` direct include からinternal header + 明示compile/link targetへ移った境界変更を確認した。命名、internal/public境界、責務分離、挙動維持、status / metadata / deadline / transport lifecycle risk、production / test boundaryをレビューした。

## Issues

- none

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Evidence

- `config.m4` はproduction extensionのcompile targetに `protocol_core.c status_core.c transport_core.c` を追加しており、runtime transportを増やしたり選択肢を戻したりしていない。
- `main.c` は `protocol_core.c` / `status_core.c` のdirect includeを外し、既存のPHP surface、transport orchestration、bench conditional includeの並びを維持している。
- `transport.c` は `transport_core.c` のdirect includeを外しつつ、HTTP/2 connection / stream lifecycle、metadata storage、deadline I/O、nghttp2 callback ownershipを引き続きtransport本体に置いている。
- `protocol_core.h` はgRPC status value、content-type、identity encoding、percent decode helper、grpc-timeout formattingに限定され、socket / nghttp2 / PHP object lifecycleを公開していない。
- `status_core.h` は `grpc_call` のstatus taxonomy helperだけを公開し、HTTP/2 connection lifecycleやPHP status object constructionを公開していない。
- `transport_core.h` はchannel identity、authority、effective option limit、input validationなどtransport setup helperに限定され、RPC response metadataやstream status stateを所有していない。
- C unit / fuzz / coverage runnerはtest sourceと対象core sourceを別translation unitとしてcompile/linkする形へ変更され、testが`.c` direct includeに依存しない境界になっている。

## Domain Model Assessment

- Naming: `protocol_core` はgRPC wire semantic helper、`status_core` はcall status taxonomy、`transport_core` はHTTP/2 transport setup helperという現在の責務と一致している。
- Internal/public boundary: 追加headerはrepository内部のC APIであり、install用public C APIやPHP surfaceを増やしていない。
- Responsibility boundary: gRPC frame/status/content-type parsing、call status classification、transport setup validationの切り出しであり、HTTP/2 connection / stream lifecycleの所有元は `transport.c` に残っている。
- Behavior preservation: 対象helperの条件分岐や定数値は移動前と同じで、変更の主効果はlink boundaryの明示化に留まっている。
- Status / metadata / deadline risk: status priority、HTTP fallback、RST_STREAM mapping、metadata byte limit、grpc-timeout formattingの所有関係は維持されている。metadata storageやdeadline enforcementのproduction pathには新しい分岐がない。
- Transport lifecycle risk: GOAWAY / EOF / RST_STREAM / connection cache lifecycleの実体は今回のcore helper header化では移動していない。
- Production vs test boundary: test runnerがcore sourceを明示linkするようになった点は、production helper APIをtestから使う境界として妥当。bench-only entrypointの公開条件も変更されていない。

## Residual Risks

- `transport_core.c` と `status_core.c` はまだ `internal.h` に依存しており、`internal.h` にはPHP surface、OpenSSL、nghttp2、module-level static stateが広く含まれる。今回のPhase 2目的からは許容できるが、後続のheader分割では `grpc_call` status viewやauthority buffer limitをより狭いinternal type/headerへ移す余地がある。
- `transport_core.h` は `php.h` / `zend_long` / `zend_ulong` に依存するため、pure C helperというよりPHP extension internal helperである。現行スコープでは問題ないが、将来の非PHP unit testやlibrary化を狙う場合は型境界を再評価する必要がある。
- このレビュー中に `docker version` はDocker socket permission errorで失敗したため、Docker内のbuild / C unit / PHPTは再実行していない。静的確認として `git diff --check` はPASSした。

## Verification

- Review-only inspection of uncommitted diff and adjacent domain docs.
- `git diff --check`: PASS.
- Docker-based test gates: not run in this review session because Docker API access returned permission denied for `/Users/daisuke/.orbstack/run/docker.sock`.
