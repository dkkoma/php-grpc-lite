---
Status: Open
Owner: Codex
Created: 2026-05-27
Branch: main
---

# grpc_lite.backend defaultをhttp2に固定する

## 目的

`grpc_lite.backend` のdefaultを `auto` から `http2` に変更し、FrankenPHP grpc-go backendを明示opt-inにする。

## 背景

現行defaultの `auto` は、`FrankenGrpc\Channel` internal classが存在すると自動で `franken-go` backendを選ぶ。FrankenPHP環境でgrpc-go extension surfaceがloadされているだけでtransportが切り替わるため、ユーザーが意図しない性能・metadata・TLS・error handling差分が発生し得る。

php-grpc-liteの通常runtime transportはnghttp2 HTTP/2 backendであり、franken-go backendはFrankenPHP環境でgrpc-go channel semanticsを使いたい場合のoptional backendとして扱う。

## スコープ

- `grpc_lite.backend` INI defaultを `http2` にする。
- per-channel / INIで `franken-go` を明示した場合の挙動は維持する。
- `auto` は互換的な明示指定値として残すが、defaultにはしない。
- README / design doc / PHPTを更新する。

## 非スコープ

- franken-go backendの削除。
- franken-go backendの性能再評価。
- backend option名の変更。

## 完了条件

- `ini_get('grpc_lite.backend') === 'http2'` になる。
- `grpc_lite.backend=franken-go` 明示指定のPHPTが通る。
- `auto` の説明がdefaultではなく明示指定値として文書化される。

## 検証

- `./tools/test/check-phpt.sh`: PASS, 16/16
- `docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- Backend selection domain review: `docs/reviews/issues/2026-05-27-default-backend-http2-domain-review.md`, Blocker/High/Medium/Low none

## 完了判断

2026-05-27に完了。`grpc_lite.backend` defaultは `http2` になり、FrankenPHP grpc-go backendは `franken-go` または明示的な `auto` 指定時だけ選択される。
