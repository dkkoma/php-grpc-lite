# Metadata control compatibility observation (2026-05-04)

## Scope

reserved / fixed headers injection と key/value validation の実測記録。
`grpc-lite curl`、`grpc-lite native`、公式 `ext-grpc` に同じ request metadata を渡し、status、例外、server が実際に見た metadata を比較した。

## Command

```sh
BENCH_TAG=metadata-control-validation-20260504 ./bench/compare-metadata-control-compat.sh
```

Saved JSON:

- `var/bench-results/phase2-metadata-control-compat-metadata-control-validation-20260504-curl-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-control-compat-metadata-control-validation-20260504-native-php-grpc-lite.json`
- `var/bench-results/phase2-metadata-control-compat-metadata-control-validation-20260504-ext-ext-grpc.json`

## Summary

| ケース | curl | native | ext-grpc | 見解 |
|---|---|---|---|---|
| `grpc-status` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | php-grpc-lite は library-owned metadata として送信前に落とす |
| `grpc-message` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | `grpc-status` と同様 |
| `grpc-timeout` metadata | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | `timeout` option からのみ生成する |
| `grpc-encoding: gzip` | OK、server app からは値なし | OK、server app からは値なし | status 12 | php-grpc-lite は user metadata から落とす。ext-grpc は request compression 指定として解釈する |
| `te` override | OK、server app からは値なし | OK、server app からは値なし | OK、server app からは値なし | fixed header として library が所有 |
| `content-type` override | server app は `application/grpc` のみ | server app は `application/grpc` のみ | server app は `application/grpc` のみ | user metadata からは落とし、固定値のみ送信 |
| `user-agent` override | fixed UA のみ | fixed UA のみ | fixed UA のみ | curl 経路の user value leak は解消済み |
| `:authority` / `:path` | `InvalidArgumentException` | `InvalidArgumentException` | `InvalidArgumentException` | pseudo header 相当は送信前 reject |
| uppercase key | OK、server app に見える | OK、server app に見える | OK、server app に見える | php-grpc-lite は送信前に lower-case normalize する |
| `_` / `.` key | OK | OK | OK | gRPC header-name範囲内 |
| space / UTF-8 key | `InvalidArgumentException` | `InvalidArgumentException` | `InvalidArgumentException` | 送信前 validation で reject |
| empty value | OK、保持 | OK、保持 | OK、保持 | curl 経路の empty value 欠落は解消済み |
| leading space value | leading space は落ちる | 保持 | 保持 | gRPC仕様上はstripされ得るため、curl stable route では非保証 |
| trailing space value | OK、保持 | OK、保持 | OK、保持 | 全経路で保持 |
| comma value | OK、保持 | OK、保持 | OK、保持 | ASCII metadataではそのまま保持 |
| CRLF value | `InvalidArgumentException` | `InvalidArgumentException` | `LogicException` | php-grpc-lite は送信前 validation で reject |
| UTF-8 value | `InvalidArgumentException` | `InvalidArgumentException` | `LogicException` | ASCII metadata value範囲外として reject |

## Implementation implications

php-grpc-lite 側の実装方針:

1. request metadata key validation
   - 許可: `0-9`, `a-z`, `A-Z`, `_`, `-`, `.`
   - normalize: uppercase は lowercase へ寄せる
   - reject: pseudo header `:*`、space、non-ASCII、その他 HTTP/2/gRPC key範囲外
2. reserved / fixed header filtering
   - filter: `grpc-status`, `grpc-message`, `grpc-encoding`, `te`, `content-type`, `user-agent`
   - `grpc-timeout` は user metadata ではなく call option `timeout` のみが生成する
3. ASCII value validation
   - reject: CR/LF、non-ASCII
   - accept: empty、comma、trailing space
   - leading space は仕様上 strip され得るため、curl stable route では保持を保証しない
4. curl parity gap after validation
   - leading space value は curl 経路では strip される
   - native default route と ext-grpc は leading space を保持する

## Next step

P1観測と request metadata validation / filtering は完了。残る細部互換は HTTP/2 header list size 境界の観測。
