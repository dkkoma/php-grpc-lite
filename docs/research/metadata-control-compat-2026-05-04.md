# Metadata control compatibility observation (2026-05-04)

## Scope

reserved / fixed headers injection と key/value validation の実測記録。
`grpc-lite curl`、`grpc-lite native`、公式 `ext-grpc` に同じ request metadata を渡し、status、例外、server が実際に見た metadata を比較した。

## Command

```sh
BENCH_TAG=metadata-control-20260504-final ./bench/phase2/compare-metadata-control-compat.sh
```

Saved JSON:

- `var/bench-results/phase2-metadata-control-compat-metadata-control-20260504-final-curl-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-control-compat-metadata-control-20260504-final-native-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-control-compat-metadata-control-20260504-final-ext-ext-grpc.json`

## Summary

| ケース | curl | native | ext-grpc | 見解 |
|---|---|---|---|---|
| `grpc-status` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | gRPC system metadata として app metadata には出ない。user metadata としては送信前 reject が安全 |
| `grpc-message` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | `grpc-status` と同様 |
| `grpc-timeout` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | library option が所有すべき。user metadata では reject/ignore 対象 |
| `grpc-encoding: gzip` | status 12 | status 12 | status 12 | request compression 指定として解釈され、server が gzip decompressor 不在で失敗 |
| `te` override | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | fixed header として library が所有 |
| `content-type` override | server app は `application/grpc` のみ | server app は `application/grpc` のみ | server app は `application/grpc` のみ | override は効かない。user metadata では reject/ignore 対象 |
| `user-agent` override | fixed UA + user value の2値が app に見える | fixed UA のみ | fixed UA のみ | curl 経路だけ user value が漏れる。filter が必要 |
| `:authority` / `:path` | actual pseudo value or empty、成功 | actual pseudo value or empty、成功 | `InvalidArgumentException` | pseudo header 相当は送信前 reject が妥当 |
| uppercase key | OK、server app に見える | OK、server app に見える | OK、server app に見える | gRPC keyは lower-case normalize 方針を決める必要あり |
| `_` / `.` key | OK | OK | OK | gRPC header-name範囲内 |
| space / UTF-8 key | status 14 / protocol error | status 14 / stream reset | `InvalidArgumentException` | 送信前 validation で reject する |
| empty value | curl は server app 値なし | native は empty value あり | ext-grpc は empty value あり | curl 経路で empty value preservation が不足 |
| leading space value | curl/native は leading space が落ちる | curl/native は leading space が落ちる | ext-grpc は保持 | gRPC仕様上はstripされ得るが、経路差あり |
| trailing space value | OK、保持 | OK、保持 | OK、保持 | 全経路で保持 |
| comma value | OK、保持 | OK、保持 | OK、保持 | ASCII metadataではそのまま保持 |
| CRLF value | curl bad argument / status 14 | stream reset / status 14 | `LogicException` | 送信前 validation で reject する |
| UTF-8 value | OK、server app に見える | OK、server app に見える | `LogicException` | ASCII metadata value範囲外。送信前 validation で reject する |

## Implementation implications

観測結果から、php-grpc-lite 側で実装方針を決めるべき点:

1. request metadata key validation
   - 許可: `0-9`, `a-z`, `A-Z`, `_`, `-`, `.`
   - normalize: uppercase は lowercase へ寄せる
   - reject: pseudo header `:*`、space、non-ASCII、その他 HTTP/2/gRPC key範囲外
2. reserved / fixed header filtering
   - reject or ignore: `grpc-status`, `grpc-message`, `grpc-encoding`, `te`, `content-type`, `user-agent`
   - `grpc-timeout` は user metadata ではなく call option `timeout` のみが生成する
3. ASCII value validation
   - reject: CR/LF、non-ASCII
   - accept: empty、comma、trailing space
   - leading space は仕様上 strip され得るが、curl/native/ext-grpcで差があるため方針決定が必要
4. curl parity gap
   - empty value が curl 経路で欠落する
   - `user-agent` user metadata が curl 経路だけ app metadata に漏れる

## Next step

P1観測は完了。次は上記 policy を `docs/SPEC.md` に確定し、curl/native両経路で送信前 validation / filtering を実装する。
