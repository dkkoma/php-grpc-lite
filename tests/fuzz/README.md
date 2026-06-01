# Fuzz corpus

このdirectoryのfuzz targetは意図的に小さくしている。PHPTだけでは網羅しにくいpure C protocol helperのboundary valueを守るためのもの。

## Harness

`fuzz_protocol_core.c` は、入力の先頭byteで対象helperを切り替える。

| Selector | Target |
|---:|---|
| `0` | `grpc_protocol_parse_status_value()` |
| `1` | `grpc_protocol_is_valid_content_type()` |
| `2` | `grpc_protocol_is_identity_encoding()` |
| `3` | `grpc_lite_hex_value()` |
| `4` | fuzzされたbuffer lengthとtimeoutを使う `grpc_lite_format_timeout_us()` |
| other | 全helperをまとめて通すsmoke |

harnessは副作用なしを維持する。socket、PHP runtime object、nghttp2 session、OpenSSL objectは使わない。

## Seed corpus

`tests/fuzz/corpus/protocol_core/` には、安定して残したいboundary caseのnamed seedを置く。

| Seed | Purpose |
|---|---|
| `status-ok` | 1桁のvalid gRPC status |
| `status-max` | validな最大gRPC status |
| `content-type-basic` | 基本形の `application/grpc` content-type |
| `content-type-proto` | `application/grpc+proto` content-type |
| `encoding-identity` | 受理するidentity encoding |
| `timeout-large` | 大きいtimeout値のformatting path |

runnerは実行前にcommitted seedを `var/fuzz/corpus/` へcopyする。fuzzerが見つけた入力やcrash artifactは `var/fuzz/` 配下に残し、committed corpusへ自動では戻さない。

## Runner

実行コマンド:

```bash
./tools/test/check-crash-ub.sh
```

`FUZZ_RUNS` はdeterministic smoke runの回数を制御する。CIとCrash/UB checkでは短く保つ。長時間のfuzz campaignは手動で実行し、明確な新しいboundary seedを追加できる場合だけcommitする。
