---
Status: Open
Owner: Codex
Created: 2026-05-18
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/4
---

# GitHub issue #4: steady-state gRPC trafficでのbrk(2) grow/shrink

## 目的

php-grpc-lite 0.0.4のSpanner-heavy workloadで観測された、RPCごとの `brk(2)` grow/shrink が現在版でも残るかを確認し、残る場合はC拡張側の不要なheap churnを特定して削減する。

## 背景

GitHub issue #4では、PHP-FPM workerに対する `strace -e trace=brk` で、php-grpc-lite 0.0.4がsteady-state gRPC traffic中にheapを繰り返しgrow/shrinkする一方、同条件のext-grpc 1.58では `brk` が0回だったと報告されている。

報告値:

| implementation | brk calls | mmap / munmap |
| --- | ---: | ---: |
| ext-grpc 1.58 | 0 | 136 / 136 |
| php-grpc-lite 0.0.4 | 223 | 111 / 110 |

1回の `brk` は2〜20us程度で単独の支配要因ではないが、高負荷時のCPU効率やallocator lock/cache localityに影響する可能性がある。

## スコープ

- 現在版で `brk(2)` churnが再現するか確認する。
- 0.0.4との差分を見て、既に改善済みの可能性を切り分ける。
- C拡張内のper-RPC allocation / freeパターンを洗い出す。
- すぐ直せるhotpath allocationがあれば別commitで対応する。

## 非スコープ

- PHP Zend allocator全体の置換。
- glibc malloc tunableの本番推奨化。
- ext-grpcの実装模倣。

## 計画

- ローカルDocker環境で `strace -e trace=brk` を使い、0.0.4 SOとcurrent SOのTLS small RPC workloadを比較する。
- `ext/grpc/*.c` の `emalloc` / `safe_emalloc` / `pemalloc` / `smart_str` / `zend_hash` / `zval` hotpathを確認する。
- 0.0.5で導入済みのpersistent connection entry / TLS write buffer変更がissue #4の観測に効いているか判断する。

## 進捗

- GitHub issue #4を確認し、本issueを作成。

## 完了条件

- issue #4の観測が現在版で再現するかを説明できる。
- 原因候補を優先度つきで整理する。
- 直せるものは修正し、検証結果を記録する。

## 調査結果 2026-05-18

### GitHub issue確認

- 対象GitHub issue: #4 `[perf] Frequent brk(2) grow/shrink during steady-state gRPC traffic`
- 報告対象: php-grpc-lite 0.0.4 / PHP-FPM / real Google Cloud Spanner / TLS gRPC / Spanner-heavy workload
- 報告内容: single-concurrency 60秒でphp-grpc-liteのみ `brk` が223回、ext-grpc 1.58は0回

### ローカルCLI steady-state確認

`php` CLI processをwarmup後に `strace -e trace=brk -p <pid>` でattachし、TLS unaryを10,000 calls実行した。

| workload | 0.0.4 brk count | current brk count | 備考 |
| --- | ---: | ---: | --- |
| reused TLS client, 10,000 unary calls | 0 | 0 | warmup後attach。startup noiseを除外。 |
| per-call new TLS client, 1,000 unary calls | 0 | 0 | Channel object再生成を含む。 |

結論: 単純なCLI steady-stateでは0.0.4/currentともに再現しない。

### ローカルFPM request lifecycle確認

php-fpmを `strace -f -e trace=brk` 配下で起動し、FastCGI requestを500回実行した。

| workload | current brk count | 備考 |
| --- | ---: | --- |
| h2c small unary via FPM request | 27 | php-fpm startup / worker spawn時のgrowのみ。request loop中のgrow/shrink patternは見えない。 |
| TLS small unary via FPM request | 30 | 同上。TLSでもrequest loop中のgrow/shrink patternは見えない。 |

最初の `ptrace -p <worker>` attachはcontainer権限で `Operation not permitted` だったため、php-fpm自体をstrace配下で起動する方式に切り替えた。

### コード調査

`ext/grpc/*.c` / `*.h` のallocator利用を確認した。

- php-grpc-lite自身の直接 `malloc` / `free` はない。
- `pemalloc` / `pecalloc` は `transport.c` のpersistent connection / TLS write buffer / h2_connection確保に限定され、per-RPC hotpathには見えない。
- per-RPC hotpathのC側allocationは主に `emalloc` / `zend_string_init` / `smart_str` / PHP array metadata生成で、これはZend memory manager側に乗る。
- nghttp2 sessionは `nghttp2_session_client_new()` を使っており、nghttp2内部allocationはデフォルトallocator、つまりlibc `malloc/free` 側に出る可能性がある。
- nghttp2には `nghttp2_session_client_new3(..., nghttp2_mem *mem)` があり、custom allocatorを渡せる。ただしpersistent HTTP/2 sessionがrequestをまたぐため、Zend request allocatorをそのまま使うのは危険。採用するならconnection lifetimeに紐づくpool/freelist設計が必要。

### 現時点の解釈

- ローカルの制御可能なCLI/FPM small RPCでは、issue #4のsteady-state grow/shrink patternは再現しない。
- 報告はreal Spanner + google/cloud-spanner + PHP-FPM request lifecycle + glibc mallocの組み合わせで出ている可能性が高い。
- php-grpc-lite 0.0.5ではissue #2/#3に対応済みでTLS write/read syscall shapeが変わっているため、0.0.4のbrk観測がそのまま残るかは未確認。
- 残る場合の第一候補はnghttp2内部のper-stream/per-frame allocationがglibc allocatorへ出ている経路。第二候補はOpenSSL / TLS record処理やHPACK dynamic table更新に伴うlibc allocation。第三候補はGAX / google/cloud-spanner側のrequest lifecycle allocationがphp-grpc-lite pathでだけ高水位を超えてtrimを誘発しているケース。

## 次の確認候補

1. real Spanner workloadで0.0.5を再計測し、issue #4の `brk` grow/shrink patternが残るか確認する。
2. 残る場合、`strace -f -tt -e trace=brk,mmap,munmap` とrequest markerを同時に取り、request boundary / RPC boundary / malloc trimのどこに同期しているか見る。
3. `ltrace` や `LD_PRELOAD` malloc tracer、またはeBPF uprobesで `malloc/free/realloc` のcallerを取り、nghttp2 / OpenSSL / PHP core / extensionのどこがlibc allocatorを叩いているか切り分ける。
4. nghttp2 custom allocatorによるconnection-local pool/freelist PoCを別branchで試し、brkだけでなくCPU/latencyが改善するか判断する。

## 判断

- Status: Open
- 判断: ローカルでは未再現。0.0.4固有、real Spanner workload固有、またはFPM/glibc allocator条件依存の可能性がある。
- 優先度: issue #2/#3より低い。ただし実負荷でCPU差が残る場合は、nghttp2 allocator / connection-local pool PoCを検討する価値がある。
