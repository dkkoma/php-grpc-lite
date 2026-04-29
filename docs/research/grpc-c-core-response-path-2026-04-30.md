# grpc/grpc C-core response path survey 2026-04-30

large response / payload-unary の構造差分を読むため、公式 `grpc/grpc` の receive path と現在の php-grpc-lite の response path を比較したメモ。`_research/grpc` は scratch clone なのでコミットしない。

## 調査対象

| 項目 | 内容 |
|---|---|
| upstream | `https://github.com/grpc/grpc` |
| checkout | shallow clone, 2026-04-30 時点 |
| 主な確認ファイル | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h` |
|  | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc` |
|  | `_research/grpc/src/core/lib/surface/call_utils.h` |
|  | `_research/grpc/src/php/ext/grpc/byte_buffer.c` |
|  | `src/Grpc/UnaryCall.php` |
|  | `src/Grpc/ServerStreamingCall.php` |

## C-core の receive/message assemble

HTTP/2 DATA frame と gRPC message は 1:1 とは限らない。C-core 側にもその前提が明示されており、1 gRPC message が複数 DATA frame にまたがること、1 DATA frame に複数 message が入ることの両方を扱う。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| DATA frame 入力 | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc:277` | stream の `GrpcMessageAssembler` を取り出す |
| payload move | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc:278` | `AppendNewDataFrame(frame.payload, frame.end_stream)` |
| internal buffer | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:59` | DATA payload を `message_buffer_` に move する |
| header parse | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:87` | `ExtractGrpcHeader(message_buffer_)` で 5B header を読む |
| payload move | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:110` | complete message の payload を `Message` の `SliceBuffer` に move する |
| upper stack | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc:299` | complete message を `SpawnPushMessage()` で上位へ渡す |

C-core は response body 全体を巨大な contiguous byte string として組み立てるより、DATA frame payload を `SliceBuffer` として移動し、complete message 単位で payload slice を上位へ渡す構造になっている。

## ext-grpc の PHP string 化

ext-grpc は最終的に PHP runtime へ serialized protobuf string を返す必要があるため、C-core の `grpc_byte_buffer` を `zend_string` にコピーする。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| recv op setup | `_research/grpc/src/core/lib/surface/filter_stack_call.cc:985` | `GRPC_OP_RECV_MESSAGE` で受信 buffer pointer を保持 |
| byte buffer 作成 | `_research/grpc/src/core/lib/surface/call_utils.h:565` | `grpc_raw_byte_buffer_create(nullptr, 0)` |
| payload move | `_research/grpc/src/core/lib/surface/call_utils.h:567` | `message->payload()` の slice buffer を `grpc_byte_buffer` に move |
| PHP ext receive | `_research/grpc/src/php/ext/grpc/call.c:499` | `byte_buffer_to_zend_string(message)` |
| zend string alloc | `_research/grpc/src/php/ext/grpc/byte_buffer.c:41` | message length の `zend_string_alloc()` |
| slice copy | `_research/grpc/src/php/ext/grpc/byte_buffer.c:46` | slice を順に `memcpy()` して PHP string に詰める |

ここから、large response で ext-grpc も最終段に payload size 分の contiguous PHP string copy を持つことが分かる。違いは、その前段の HTTP/2 DATA frame assemble と gRPC frame parse が C-core の slice buffer で進む点にある。

## php-grpc-lite の response path

現在の unary path は libcurl write callback で受けた body chunk を list に積み、parse 時に必要なら `implode()` で body を組み立て、5B header を読んで payload を `substr()` する。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| libcurl callback | `src/Grpc/UnaryCall.php:281` | `CURLOPT_WRITEFUNCTION` が chunk を受ける |
| chunk list | `src/Grpc/UnaryCall.php:287` | `$this->bodyChunks[] = $chunk` |
| body assemble | `src/Grpc/UnaryCall.php:298` | 1 chunk ならそのまま、複数なら `implode()` |
| header parse | `src/Grpc/UnaryCall.php:162` | `unpack('N', substr($body, 1, 4))` |
| payload copy | `src/Grpc/UnaryCall.php:168` | `substr($body, 5, $len)` で payload string を作る |
| protobuf decode | `src/Grpc/UnaryCall.php:174` | PHP protobuf runtime へ渡す |

server streaming path は別構造で、callback 中に `$this->buffer .= $chunk` しながら complete frame を切り出す。

| 段階 | 読んだ箇所 | 挙動 |
|---|---|---|
| chunk append | `src/Grpc/ServerStreamingCall.php:254` | `onBodyChunk()` |
| buffer concat | `src/Grpc/ServerStreamingCall.php:257` | `$this->buffer .= $chunk` |
| frame parse | `src/Grpc/ServerStreamingCall.php:260` | buffer offset から 5B header と payload length を読む |
| payload copy | `src/Grpc/ServerStreamingCall.php:272` | complete payload を `substr()` して pending に積む |
| consumed trim | `src/Grpc/ServerStreamingCall.php:277` | 消費済み範囲を捨てる |

## large response の構造差分

| 項目 | ext-grpc / C-core | php-grpc-lite unary |
|---|---|---|
| HTTP/2 DATA frame assemble | C-core `SliceBuffer` に move | libcurl callback が PHP string chunk を渡す |
| gRPC 5B header parse | C-core が `SliceBuffer` 先頭から読む | PHP が assembled body から `substr` + `unpack` |
| response body full assemble | C-core 内では slice buffer | 複数 chunk なら PHP `implode()` |
| payload extraction | C-core は payload slice を message へ move | PHP `substr($body, 5, $len)` で payload copy |
| PHP runtime へ返す最終 copy | ext-grpc も `zend_string_alloc` + `memcpy` あり | payload string は PHP string として作成済み |
| protobuf deserialize | PHP protobuf runtime | PHP protobuf runtime |

large response では、ext-grpc が最終的な PHP string copy を避けているわけではない。ただし HTTP/2 chunk から gRPC message payload までの途中表現は C-core の slice move 中心で、php-grpc-lite の unary は `implode()` と payload `substr()` により PHP userland の contiguous string copy が入りやすい。

## 計測結果との対応

既存の `payload-unary-diagnostic` では、large response の userland 区間は主因ではないことが見えている。100KB/1MB response で支配的だったのは `curl_exec()` 内、特に first byte 前後と download 区間であり、`body_append_ns_total`、`response_payload_slice_ns`、`response_deserialize_ns` は p99 tail の主要因ではなかった。

したがって、この調査から「php-grpc-lite の large response は `implode()` / `substr()` を直せば大きく改善する」とは言えない。分かるのは次の範囲に限る。

- ext-grpc/C-core と php-grpc-lite では、HTTP/2 DATA frame から gRPC message payload までの途中表現が違う。
- ただし現在の計測では、その違いが observed p99 の主因とは見えていない。
- large response の差分をさらに詰めるなら、PHP userland より `curl_exec()` 内の libcurl/nghttp2 receive path と、C-core chttp2 receive path の比較が必要になる。

## libcurl source は必要か

userland copy 構造を確認するだけなら必須ではない。理由は以下。

- php-grpc-lite 側で観測している userland 単位は、libcurl が PHP write callback に渡した後の chunk list / assemble / slice / deserialize で十分に分かれている。
- ext-grpc 側の PHP 境界は、C-core の `SliceBuffer` assemble と ext-grpc の `zend_string` copy まで読めば説明できる。
- 現在の large response p99 を説明したい場合は、むしろ libcurl/nghttp2 内部の `curl_exec()` receive path、DATA callback 分割、flow control、buffer size、scheduler 影響を読む必要がある。

したがって順番としては、この C-core response path は「userland 最終 copy と途中表現の差分」を押さえるための調査に留める。large response の observed tail を説明する目的なら、次は libcurl/nghttp2 または curl trace / server stats と突き合わせた transport 層の調査に進むのが妥当。

## 現時点の判断

- large response でも ext-grpc は最終的に PHP string copy を持つ。
- ext-grpc/C-core は HTTP/2 DATA frame から gRPC message payload までを slice buffer で扱うため、途中の large concat を避けやすい。
- php-grpc-lite unary は chunk list 化で callback 時の `body .= chunk` は避けたが、parse 時の `implode()` と payload `substr()` は残る。
- ただし既存計測では、これらの userland copy は large response p99 の主因ではない。
- large response の差分説明に必要なのは、libcurl/nghttp2 receive path と C-core chttp2 receive path の transport 層比較である。
