# grpc/grpc C-core upload path survey 2026-04-30

`_research/grpc` に clone した公式 `grpc/grpc` を、large request / small response の差分理由を読むために確認したメモ。clone 自体は scratch なのでコミットしない。

## 調査対象

| 項目 | 内容 |
|---|---|
| upstream | `https://github.com/grpc/grpc` |
| checkout | shallow clone, 2026-04-30 時点 |
| 主な確認ファイル | `_research/grpc/src/php/ext/grpc/call.c` |
|  | `_research/grpc/src/php/ext/grpc/byte_buffer.c` |
|  | `_research/grpc/src/core/lib/surface/filter_stack_call.cc` |
|  | `_research/grpc/src/core/ext/transport/chttp2/transport/chttp2_transport.cc` |

## ext-grpc の送信経路

PHP extension 側の `GRPC_OP_SEND_MESSAGE` は、PHP から渡された serialized protobuf string を `string_to_byte_buffer()` に渡して C-core の `grpc_byte_buffer` に変換する。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| PHP ext batch | `_research/grpc/src/php/ext/grpc/call.c:360` | `GRPC_OP_SEND_MESSAGE` の値から `message` string を取り出す |
| byte buffer 化 | `_research/grpc/src/php/ext/grpc/call.c:385` | `string_to_byte_buffer(Z_STRVAL_P(...), Z_STRLEN_P(...))` を呼ぶ |
| C slice copy | `_research/grpc/src/php/ext/grpc/byte_buffer.c:27` | `grpc_slice_from_copied_buffer()` で PHP string から C slice へコピーする |
| C-core start | `_research/grpc/src/php/ext/grpc/call.c:467` | `grpc_call_start_batch()` に `grpc_op` を渡す |
| cleanup | `_research/grpc/src/php/ext/grpc/call.c:545` | batch 完了後に `grpc_byte_buffer_destroy()` する |

ここから、ext-grpc でも PHP serialized string から C-core 所有 buffer へのコピーは発生する。つまり large request で ext-grpc が完全 zero-copy というわけではない。

## C-core の frame build

C-core surface は `grpc_byte_buffer` 内の raw slice buffer を call の `send_slice_buffer_` に move する。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| slice buffer move | `_research/grpc/src/core/lib/surface/filter_stack_call.cc:859` | `send_slice_buffer_.Clear()` 後に `grpc_slice_buffer_move_into()` |
| transport payload | `_research/grpc/src/core/lib/surface/filter_stack_call.cc:863` | transport op の `send_message.send_message` は `send_slice_buffer_` を参照 |
| gRPC 5B header | `_research/grpc/src/core/ext/transport/chttp2/transport/chttp2_transport.cc:1677` | `grpc_slice_buffer_tiny_add()` で 5B header を追加 |
| payload append | `_research/grpc/src/core/ext/transport/chttp2/transport/chttp2_transport.cc:1699` | message payload slices を `grpc_slice_buffer_add()` で追加 |

重要なのは、C-core は gRPC の 5 byte message header と payload を 1 つの巨大な連結文字列として作らないこと。header は小さい slice、payload は既存 slice ref として flow-controlled buffer に積まれる。

## php-grpc-lite との差分

現在の php-grpc-lite は、PHP userland で serialized protobuf string と 5B header を連結し、その連結済み frame を libcurl に `CURLOPT_POSTFIELDS` として渡す。

| 項目 | ext-grpc / C-core | php-grpc-lite |
|---|---|---|
| protobuf serialize | PHP protobuf runtime | PHP protobuf runtime |
| PHP string -> native boundary | `grpc_slice_from_copied_buffer()` で C slice copy | `CURLOPT_POSTFIELDS` に PHP string を渡す |
| gRPC 5B header | C-core が small slice として追加 | PHP で `"\x00" . pack('N', len) . payload` を連結 |
| payload + header の full concat | 基本なし。slice buffer に header と payload slice を積む | あり。large request では 5B header + payload の PHP string を作る |
| HTTP/2 framing | C-core chttp2 transport | libcurl/nghttp2 |
| upload completion | C-core の write callback / flow control | libcurl の `curl_exec()` 内 |

large request 診断で `request_frame_build_ns` が 1MB p99 で大きく見えたのは、この PHP userland の full concat と整合する。ext-grpc との差分は serialize そのものではなく、serialize 後に gRPC frame をどこで、どの表現で持つかにある。

## read callback 実験との関係

`CURLOPT_READFUNCTION` 実験は PHP の `$frame = header . payload` を避ける狙いだった。1MB p50 は改善したが、p99 と throughput が安定せず採用しなかった。

この upstream 調査から見ると、方向性自体は C-core の構造に近い。ただし PHP/libcurl の read callback は callback 境界と libcurl upload path の都合で、C-core の slice buffer と同じ性質にはならない。特に p99 は libcurl/nghttp2 と Docker scheduler の影響を受けるため、単純な callback 化だけでは十分に安定しない。

## PHP userland で検討できること

| 候補 | 根拠 | 注意 |
|---|---|---|
| header と payload の二分割 upload | C-core は header slice + payload slice として扱う | PHP curl で安定した scatter/gather upload を作れるかが課題 |
| request frame build の条件付き最適化 | small request では効果が薄く、large request だけ効く | 通常 unary の単純性を壊さないこと |
| diagnostics 維持 | `request_serialize_ns` / `request_frame_build_ns` / curl upload bytes で差分を追える | 改善採用は p50 だけでなく p99 と throughput を見る |

一方、libcurl/nghttp2 の内部 HTTP/2 framing や flow control の tail は PHP userland から直接制御しにくい。ここを本格的に詰めるなら、C extension 化、nghttp2 直接利用、あるいは別 transport 実装の話になる。

## 現時点の判断

- ext-grpc も PHP serialized string から C buffer へのコピーは持つ。
- ext-grpc/C-core は gRPC frame 全体の large concat を避け、header と payload slices を transport buffer に積む。
- php-grpc-lite の large request 固定費は、PHP userland の frame concat と libcurl upload 境界が主な観測対象。
- PHP のまま試すなら、C-core と同じ「header と payload を full concat しない」方向だけが筋が良い。ただし read callback 実験では p99 の安定性が足りなかったため、採用には追加の検証が必要。
