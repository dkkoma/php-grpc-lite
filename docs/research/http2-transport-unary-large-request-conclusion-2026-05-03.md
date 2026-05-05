# HTTP/2 transport unary large request PoC conclusion (2026-05-03)

## 対象

Phase 2 の large request unary call について、`php-grpc-lite` の libcurl 経路、公式 `ext-grpc`、および `poc/nghttp2-client-ext` の nghttp2 直接制御 PoC を比較した。

この PoC の目的は `ext-grpc` の数値に寄せることではなく、large request upload path の詰まりが PHP userland / libcurl / nghttp2 / server のどこにあるかを切ることだった。

## 観測結果

同一 Go test-server 条件、1MiB request / 100B response / warm sequential calls では、最新の no-copy + poll loop PoC が `php-grpc-lite` と `ext-grpc` の双方を上回った。

| implementation | calls/s | p50 | p99 | server InPayload p99 |
| --- | ---: | ---: | ---: | ---: |
| php-grpc-lite | 955.1 | 819.9μs | 4369.4μs | 3872.3μs |
| ext-grpc | 1318.8 | 482.8μs | 4071.5μs | 3831.4μs |
| nghttp2 PoC no-copy + poll loop | 1435.3 | 361.0μs | 3549.0μs | 3331.6μs |

size sweep では 100KiB / 512KiB / 1MiB / 2MiB のいずれも flow-control pause は p99 で観測されず、client upload p99 は request size に応じて増えるが、total p99 の支配要因は server InPayload 側に寄った。

## 結論

large request unary call の PoC は完了扱いでよい。

C 拡張実装に進む場合の結論は以下。

- HTTP/2 実装を自前で持つ必要はない。nghttp2 を直接使う形で十分に詰まりを外せる。
- libcurl はこの経路では外す価値がある。instrumented libcurl + nghttp2 では、libcurl data provider が約 64KiB 供給、nghttp2 が 16KiB DATA frame に分割、deferred/resume を繰り返す構造が見えた。
- C 拡張側では gRPC 5 byte header と protobuf payload を C 側 data provider に渡し、no-copy DATA frame 送信を基本形にする。
- blocking `writev` だけでは tail が残るため、partial write を保持する state machine と nonblocking poll loop が必要。
- nghttp2 の flow-control 自体は今回の warm large request p99 の主因ではなかった。client write complete 後の tail は server receive / wire / scheduler 側に寄っている。

## 実装方針

本実装では `Channel` / call lifecycle の API surface を守りつつ、transport backend として nghttp2 経路を追加するのが妥当。

最小の本実装単位は以下。

1. connection / HTTP/2 session lifecycle を PHP request 内で保持する。
2. unary request body は serialized protobuf を C 拡張に渡し、gRPC frame build と DATA 分割を C 側で行う。
3. no-copy DATA frame + partial write retry + poll loop を標準経路にする。
4. TLS / mTLS / deadline / metadata / grpc-status / trailers は libcurl 経路と同等の互換性テストを通す。

## 残課題

large request unary の client-side upload 詰まりは PoC では解消できた。一方で本実装判断には、server streaming large response の受信 path でも libcurl を外す価値があるかを別途切る必要がある。
