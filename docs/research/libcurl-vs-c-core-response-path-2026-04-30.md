# libcurl vs C-core response path survey 2026-04-30

large response で php-grpc-lite が ext-grpc に負ける区間を、実行環境の PHP ext/curl / libcurl と公式 grpc C-core の実装差から見直したメモ。

## 実行環境と調査対象

Docker `dev` の実行環境は PHP 8.4.20、libcurl 8.14.1-2+deb13u2、HTTP/2 有効。対応する upstream source として `_research/php-src` と `_research/curl` を clone して読んだ。clone 自体は scratch なのでコミットしない。

| 対象 | 読んだ箇所 |
|---|---|
| PHP `curl_exec()` | `_research/php-src/ext/curl/interface.c:2441` |
| PHP body callback trampoline | `_research/php-src/ext/curl/interface.c:566` |
| PHP `CURLOPT_RETURNTRANSFER` handling | `_research/php-src/ext/curl/interface.c:584` / `_research/php-src/ext/curl/interface.c:2473` |
| libcurl blocking perform | `_research/curl/lib/easy.c:752` |
| libcurl HTTP/2 DATA receive | `_research/curl/lib/http2.c:1463` |
| libcurl client write stack | `_research/curl/lib/transfer.c:853` / `_research/curl/lib/sendf.c:71` / `_research/curl/lib/cw-out.c:195` |
| grpc C-core receive | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc:277` / `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:59` |
| ext-grpc PHP string copy | `_research/grpc/src/php/ext/grpc/byte_buffer.c:41` |

## `curl_exec()` の内側

PHP の `curl_exec()` は、検証と cleanup を除けば `curl_easy_perform(ch->cp)` を呼ぶ。libcurl の `curl_easy_perform()` は内部で multi handle を回す blocking API であり、HTTP/2 受信、nghttp2 callback、client write callback、PHP callback trampoline までが `curl_exec()` の wall time に入る。

php-grpc-lite の `body_append_ns_total` は `src/Grpc/UnaryCall.php` の PHP callback に入った後だけを測る。したがって、以下は `curl_exec()` 内に含まれるが `body_append_ns_total` には含まれない。

- libcurl が socket/TLS から bytes を読む時間。
- nghttp2 が HTTP/2 frame を parse する時間。
- libcurl の writer stack が DATA payload を callback へ流す時間。
- PHP ext/curl が libcurl buffer から `ZVAL_STRINGL()` で PHP string を作る時間。
- PHP ext/curl が `zend_call_known_fcc()` で user callback を呼ぶ時間。

この未計測部分は large response で重要になる。1MB cached payload では body chunk p99 が 130、max chunk p50 が 16375B だったため、PHP callback trampoline と per-chunk PHP string 化が 100 回以上発生する。

## libcurl response path

libcurl の HTTP/2 path は、nghttp2 の DATA chunk callback で受けた bytes をそのまま client write stack へ渡す。

| 段階 | 実装 | 挙動 |
|---|---|---|
| socket/TLS -> h2 parser | `_research/curl/lib/http2.c:681` | `nghttp2_session_mem_recv()` に pending input を渡す |
| DATA callback | `_research/curl/lib/http2.c:1463` | `on_data_chunk_recv()` が stream と payload を受ける |
| response write | `_research/curl/lib/http2.c:1493` | `h2_xfer_write_resp(..., mem, len, false)` |
| transfer write | `_research/curl/lib/transfer.c:853` | body として `Curl_client_write()` へ渡す |
| writer stack | `_research/curl/lib/sendf.c:71` | `Curl_cwriter_write()` で writer chain を進める |
| output writer | `_research/curl/lib/cw-out.c:222` | 最大 `CURL_MAX_WRITE_SIZE` 単位で callback を呼ぶ |
| PHP callback | `_research/php-src/ext/curl/interface.c:595` | `ZVAL_STRINGL()` で chunk を PHP string 化して user callback を呼ぶ |

libcurl は gRPC message を知らない。HTTP/2 DATA payload を chunk としてアプリ callback に渡すだけで、gRPC 5B header や message boundary は php-grpc-lite の userland が後段で処理する。

## C-core response path

C-core は HTTP/2 DATA frame payload を gRPC message assembler に入れ、complete message ができるまで C-core 内の `SliceBuffer` で持つ。

| 段階 | 実装 | 挙動 |
|---|---|---|
| DATA frame | `_research/grpc/src/core/ext/transport/chttp2/transport/http2_client_transport.cc:277` | stream の `GrpcMessageAssembler` を使う |
| payload move | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:73` | DATA payload を `message_buffer_` に move |
| gRPC header parse | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:87` | 5B header を C-core 内で読む |
| payload move | `_research/grpc/src/core/ext/transport/chttp2/transport/message_assembler.h:110` | complete payload を `Message` の `SliceBuffer` へ move |
| PHP string copy | `_research/grpc/src/php/ext/grpc/byte_buffer.c:41` | ext-grpc 境界で `zend_string_alloc()` + `memcpy()` |

ext-grpc も最終的には PHP serialized message string を作るため payload size 分の copy はある。しかし DATA chunk ごとに PHP user callback を呼ばない。C-core は gRPC message boundary を知っているので、message complete 後に PHP 境界へ一度だけ出せる。

## 構造差分とボトルネックへの現れ方

| 観点 | php-grpc-lite / libcurl | ext-grpc / C-core | 現れ方 |
|---|---|---|---|
| protocol awareness | libcurl は HTTP/2 まで、gRPC message は userland | C-core が gRPC message まで処理 | php-grpc-lite は `curl_exec()` 後に frame parse が必要 |
| response chunk delivery | DATA chunk ごとに PHP callback | C-core 内で slice buffer assemble | large response で callback 回数が payload/chunk size に比例 |
| PHP string 化 | chunk ごとに `ZVAL_STRINGL()`、その後 chunk list / `implode()` / `substr()` | complete message を `zend_string_alloc()` + `memcpy()` | php-grpc-lite は callback trampoline と複数 PHP string を持つ |
| measured userland | callback 内 append / parse / deserialize | ext-grpc 内部は取れない | php-grpc-lite の `body_append_ns_total` は callback 前の PHP ext/curl cost を含まない |
| trailer handling | PHP header callback では trailer 種別が落ちるため body callback で `bodyStarted` を立てる | C-core は metadata/trailer を区別して扱う | `CURLOPT_WRITEFUNCTION` を外す fast path は trailer 判定の再設計が必要 |

既存計測では `body_append_ns_total`、payload slice、deserialize は 1MB でも p99 数十μsで、observed tail の主因ではなかった。一方、1MB cached payload の total p99 は php-grpc-lite 3907.0μs、ext-grpc 2488.1μsで、server `OutPayload` p99 は近い。差分は client stack に残る。

この調査で新しく重要になった点は、`curl_exec()` 内に「PHP ext/curl の per-DATA callback cost」が含まれること。これは現在の diagnostics では直接分解できていない。

## php-grpc-lite が ext-grpc に勝つための候補

### 1. Unary response receive fast path: body `WRITEFUNCTION` を外す

通常 unary では `CURLOPT_WRITEFUNCTION` を設定せず、`CURLOPT_RETURNTRANSFER` による PHP ext/curl の C-level `smart_str` accumulation に任せる。PHP user callback を DATA chunk ごとに呼ばないため、以下を消せる可能性がある。

- chunk ごとの `ZVAL_STRINGL()`。
- chunk ごとの `zend_call_known_fcc()`。
- `bodyChunks[]` への append。
- parse 前の `implode()`。

ただし、これはそのままでは入れられない。現在の trailer 判定は body callback が `bodyStarted` を立てることに依存している。libcurl 内部では `CLIENTWRITE_TRAILER` として trailer を区別しているが、PHP の `CURLOPT_HEADERFUNCTION` callback にはその種別が渡らない。gRPC では custom trailing metadata もあり得るため、`grpc-status` だけで trailer block を判定すると互換性を壊すリスクがある。

検証するなら、まず fast path を opt-in diagnostic として実装し、trailer block 判定を HTTP/2 header block の blank line / `grpc-status` / header ordering でどこまで安全に再構成できるかを互換テストする。通常 path へ採用する条件は、binary metadata と custom trailers を壊さず、100KB/1MB cached payload で p50/p99 が改善すること。

### 2. PHP ext/curl callback cost を計測で分離する

実装判断の前に、`curl_exec()` から現在の PHP callback 内計測を引いた残りをより明示する。特に body chunk count と `curl_exec`、`body_append_ns_total`、`response_body_assemble_ns`、`response_payload_slice_ns` を同じ表で出し、per-chunk callback overhead の上限を推定する。

本当に fast path が効くなら、1MB の 130 chunk 条件で効果が出るはず。逆に効果が出ない場合、支配要因は PHP callback ではなく libcurl/nghttp2 receive / scheduler / TLS / flow control 側に寄る。

### 3. Large request は別軸で upload path を再検討する

large request では PHP userland の frame concat と PHP ext/curl の `CURLOPT_COPYPOSTFIELDS` が効く。PHP ext/curl は string `CURLOPT_POSTFIELDS` に対して `CURLOPT_COPYPOSTFIELDS` を使うため、php-grpc-lite の `$frame = header . payload` に加えて libcurl 側 copy も入る。

read callback 実験では 1MB p50 / throughput は改善したが p99 が悪化したため採用しなかった。ext-grpc に勝つ主戦場を large request に置くなら、単純 read callback ではなく、HTTP/2 upload path の安定性まで含めて再設計が必要になる。

### 4. Metadata path は局所改善に留める

metadata-heavy では header callback / append に改善余地があるが、large payload tail の主因ではない。ext-grpc に勝つ決定打ではなく、実用上の局所最適化として扱う。

## 優先順位

| 優先 | 候補 | 理由 | リスク |
|---:|---|---|---|
| 1 | unary response fast path を diagnostic 実装 | large response 差分に直結する未計測 callback cost を消せる可能性がある | trailer/custom metadata 判定を壊す可能性 |
| 2 | callback cost の推定を bench output に追加 | fast path の採否判断ができる | 直接改善ではない |
| 3 | large request upload path の再設計 | 1MB request では ext-grpcとの差が明確 | 既存 read callback は p99 悪化済み |
| 4 | metadata append 最適化 | 実用上の小改善 | payload tail には効かない |

## 現時点の結論

php-grpc-lite が ext-grpc に勝つために PHP userland で最も筋が良い候補は、通常 unary response で `CURLOPT_WRITEFUNCTION` を外す fast path である。これは libcurl/nghttp2 を捨てずに、ext-grpc/C-core に近い「message complete まで PHP user callback に出さない」構造へ寄せる案になる。

ただし、gRPC trailer 互換性が採用条件になる。ここを安全に解けないなら、large response で ext-grpc に勝つには PHP 実装の小手先ではなく、C extension / nghttp2 直接利用 / 別 transport の領域に入る。
