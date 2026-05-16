# request metadata COW zval/domain review 2026-05-16

## Scope

- `ext/grpc/bridge.c`
- `ext/grpc/tests/020-request-metadata-control.phpt`
- `docs/issues/open/2026-05-16-request-metadata-cow-hotpath.md`

## Reviewer Role

- PHP extension zval/COW and gRPC metadata domain reviewer

## Review Prompt Summary

- 未コミット変更について、PHP extension zval/COW semantics、COW separation before writes、request/response metadata compatibility、franken-go backend影響、PHPT assertionのdomain妥当性を確認した。production codeは変更しない。

## Issues

- none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## No-Issue Safety Notes

- `grpc_lite_copy_metadata()` は request metadata の保存を `ZVAL_COPY()` に変えており、`Grpc\Call::startBatch()` 後にuserland側のmetadata配列が変更されても、PHP array COWにより `call->metadata` のtop-level HashTableとnested value arraysはwrite時に分離される。`zval_ptr_dtor(dest)` 後にarray/non-arrayを処理するため、既存のempty-array fallbackも維持されている。
- C extension側のwrite pathは `grpc_lite_append_user_agent()` と `grpc_lite_merge_call_credentials_metadata()` で `SEPARATE_ARRAY()` を行ってからtop-level metadataへ書き込む。既存value配列の中身は書き換えず、request header変換はread-onlyなので、COW shared metadataへの未分離writeは見当たらない。
- native nghttp2経路では `append_custom_request_headers()` が `call->metadata` を同期的に読み取り、string/base64 copiesをrequest header storageへ持つ。`startBatch()` と実送信の間にuserland metadataを変更するケースでも、read対象は保存時点のCOW snapshotになる。
- franken-go backendではcall credentials merge後に `grpc_lite_append_user_agent()` がmetadataを分離してからuser-agentを追加し、`ZVAL_COPY()` でPHP側 `start()` に渡す。backend実装が引数metadataを変更してもPHP COWで `call->metadata` とは分離されるため、今回の変更でuserland入力配列やcall stateを直接汚染する経路は見当たらない。
- response metadata/status metadataは、transport由来では `grpc_protocol_copy_metadata_map()` が新規配列を作り、bridge内のevent返却は `grpc_lite_add_event_metadata()` がHashTable copyを継続している。franken-go由来のmetadataだけは `grpc_lite_copy_metadata()` によりCOW共有されるが、PHP公開後のwriteはCOW分離されるため互換shapeである `array<string, list<string>>` は維持される。
- 追加PHPTはraw `Grpc\Call::startBatch()` のofficial wrapper境界で、`OP_SEND_INITIAL_METADATA` 後かつ `OP_RECV_*` 前に元metadataを変更し、Go test-serverがwireで観測したmetadataをinitial metadataとして返す値を検証している。これは内部refcountやHashTable identityではなく、送信metadataのsnapshot semanticsをwire-visibleに見るため、domain-validなassertionと判断する。
- 既存の `020-request-metadata-control.phpt` と `MetadataCompatibilityTest` がrequest validation/filtering、duplicate/list values、binary metadata、initial/trailing metadata shapeを既に扱っており、今回のCOW変更によるresponse metadata surfaceの明確な互換性欠落は見つからない。
