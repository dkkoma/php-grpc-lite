# お手本化: connection / stream ownership model整理

- Status: Open
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

現在のHTTP/2 connection / stream ownershipは、`active_stream_count`、`stream_owner_count`、`connection_owned`、`detached_from_cache`、nghttp2 stream user data、server streaming resource destructorの組み合わせで成り立っている。

実装としては実機検証済みだが、所有権のinvariantが複数関数にまたがっており、C/PHP拡張のお手本としては読者がコードから状態遷移を復元する必要がある。

## Goals

- connection / stream / call / server streaming resourceの所有権モデルを明文化する。
- active stream registration / unregister / owner clear / detached destroyのinvariantを読みやすくする。
- unaryとserver streamingで `call->connection` を残す/消す差を明確にする。
- persistent cache entry削除後のconnection lifetimeを安全かつ読みやすい形にする。

## Non-Goals

- `grpc_call` field分割。
- response parser分離。
- persistent connection reuse semanticsの変更。
- performance改善。

## Plan

1. 現在のownership state machineをドキュメント化する。
2. `register_grpc_call_stream()`、`clear_connection_call_owner()`、`clear_connection_server_streaming_call_state_owner()`、`destroy_detached_connection_if_unowned()` 周辺のinvariantを棚卸しする。
3. 小さいowner helper APIへ寄せる案を作る。
4. 実装する場合は、cleanup path、error path、resource destructor pathをPHPTで確認する。
5. domain model reviewを実施する。

## Performance Notes

所有権helper化はcleanup/error path中心なら性能影響は小さいが、active stream bookkeepingはRPC hot pathでも触るため、実装内容によってはbefore/after計測を行う。

計測候補:

- `spanner-shape`
- unary 100B
- server streaming small messages
- PHPT lifecycle/control semantics

## Progress

- 2026-05-31: 親issueからownership model作業を子issue化。

## Verification

- `./tools/test/check-phpt.sh`
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`
- `./tools/test/check-c-static-analysis.sh`
- 必要に応じてrepresentative benchmark before/after
- HTTP/2/gRPC domain model review

## Decision Log

- 2026-05-31: 所有権モデル整理は教材価値が高いが、hot pathに触る場合は性能計測対象にする。

## Close Criteria

- ownership state machineがdocsまたは小さいAPIとして読める。
- unary / server streaming / cancel / GC drop / channel close / GOAWAY / RST pathで所有権が説明できる。
- 必要な検証とdomain model reviewが完了している。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
