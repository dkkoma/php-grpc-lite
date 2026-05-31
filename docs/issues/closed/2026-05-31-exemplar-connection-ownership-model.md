# お手本化: connection / stream ownership model整理

- Status: Closed
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

### Benchmark Policy

docs/state machine化だけならruntime benchmarkは不要。実装で `register_grpc_call_stream()`、`unregister_grpc_call_stream()`、nghttp2 stream user data、`stream_owner_count`、`connection_owned`、cache detach条件のいずれかを変える場合は、before/afterを必須にする。

既存benchmarkでまず見るもの:

- `spanner-shape`
- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500`
- `./bench/run.sh throughput-unary --duration=3 --payload-bytes=100`
- `./bench/run.sh throughput-streaming --duration=3 --message-count=100 --payload-bytes=100`

既存benchmarkで足りない場合に追加するcase:

- `cpu-micro` に `tiny_unary_0b` と `tiny_streaming_1x0b` を追加し、payload処理よりstream registration / close固定費を見やすくする。
- server streaming resource destructorやGC dropに触る場合は、性能benchmarkとは別に lifecycle stress をgate化するか、measurement-onlyとして結果をissueに記録する。
- connection cache detach条件に触る場合は、reused clientとper-call clientを分けて見る。`cpu-micro` の `new_client_*` が足りなければ、connection reuse countを観測する小caseを追加する。

採否は、代表benchでregressionがないことに加え、ownership invariantが読みやすくなることを条件にする。性能差が測定誤差内で、コードが複雑になるだけなら採用しない。

## Progress

- 2026-05-31: 親issueからownership model作業を子issue化。
- 2026-05-31: `docs/grpc-call-exchange-state.md` にconnection / stream ownership節を追加し、registration / unregistration、unary、server streaming、cache detach/destroy、GOAWAY/RST_STREAMの所有権状態を既存のfield mapへ統合。
- 2026-05-31: 実装helper化は行わない。現状ではdocs/state machine化で完了条件を満たすと判断。

## Verification

- `git diff --check`: PASS
- docs-only。`register_grpc_call_stream()`、`unregister_grpc_call_stream()`、owner clear、cache detach条件、runtime pathは変更していないためPHPT/PHPUnit/benchmarkは不要。
- HTTP/2/gRPC domain model review: docs-only ownership reviewとして、connection / stream / call / resource scope、RST_STREAM / GOAWAY lifecycle、persistent cache detachとdestroy条件を確認。実装変更なしのためBlocker / High / Medium / Lowはnone。

## Decision Log

- 2026-05-31: 所有権モデル整理は教材価値が高いが、hot pathに触る場合は性能計測対象にする。
- 2026-05-31: このissueではhelper化を採用しない。`active_stream_count` と `stream_owner_count` の意味、unaryとserver streamingの `call->connection` の違いをdocsで明確にする方が、現時点ではお手本として読みやすい。
- 2026-05-31: persistent cacheの分離は `transport header boundary` issueでreject済み。ここではcache entry削除後のconnection lifetimeをstate machineとして説明する。
- 2026-05-31: 新規docを増やすと既存の `grpc_call` field mapと重複するため、独立した `connection-stream-ownership.md` は作らず既存docへ統合する。

## Close Criteria

- ownership state machineがdocsまたは小さいAPIとして読める。
- unary / server streaming / cancel / GC drop / channel close / GOAWAY / RST pathで所有権が説明できる。
- 必要な検証とdomain model reviewが完了している。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
