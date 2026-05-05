# Native mux / event loop spike decision (2026-05-05)

## 目的

同一 PHP process / thread 内で、1つのHTTP/2 session上に複数RPC streamを同時に載せる価値があるかを確認した。

processをまたぐHTTP/2 session共有は対象外。公式 ext-grpc もprocess間でHTTP/2 sessionを共有しないため、この検討ではFPM worker / FrankenPHP worker内のconcurrent useに絞った。

## spike branch

- branch: `spike/native-mux-event-loop`
- commit: `d6386cd Native transport mux spike: 同一HTTP/2 sessionで複数RPCを扱う`

spikeでは以下を実装して検証した。

- nghttp2 `session user_data` を `h2_channel*` に固定する。
- 各RPCの `grpc_call*` を `nghttp2_submit_request()` の `stream_user_data` と `data_provider.source.ptr` に保持する。
- DATA / HEADERS / stream close callback は `stream_id` から対象 `grpc_call*` を引く。
- server streamingを開いたまま同一channelで別server streaming / unaryをsubmitできる。
- completed / cancelled streamでは `nghttp2_session_set_stream_user_data(..., NULL)` を呼び、破棄済みresourceへのdangling参照を残さない。
- generatorが途中破棄されたserver streamingは `finally` でnative streamをcancelする。

## 検証結果

spike branch上で以下を確認した。

- PHPUnit: `83 tests, 287 assertions, 1 skipped`
- 同一HTTP/2 session上でserver streamingを2本openできる。
- server streamingをopenしたまま同一channelでunaryを実行できる。
- 片方のstream cancel後も別stream / 次RPCを継続できる。

mainとの差分:

| case | main native | mux spike native | 差分 |
|---|---:|---:|---:|
| warm unary | `36.891μs` | `41.795μs` | `+4.904μs` / `+13.3%` |
| server streaming 1000 | `1.244ms` | `1.322ms` | `+0.078ms` / `+6.3%` |

ext-grpc比較:

| case | mux spike native | ext-grpc |
|---|---:|---:|
| warm unary | `41.795μs` | `73.924μs` |
| server streaming 1000 | `1.322ms` | `2.798ms` |

結果ファイル:

- `var/bench-results/warm-20260505-mux-spike-php-grpc-lite.json`
- `var/bench-results/warm-20260505-mux-spike-ext-grpc.json`
- `var/bench-results/stream-smoke-20260505-mux-spike-php-grpc-lite.json`
- `var/bench-results/stream-smoke-20260505-mux-spike-ext-grpc.json`

## 判断

mainには採用しない。

理由:

- 現行public PHP APIは `UnaryCall::wait()` / `ServerStreamingCall::responses()` が同期blockingで、通常のFPM / FrankenPHP worker利用では同一実行コンテキスト内の複数in-flight RPCが自然には発生しない。
- persistent channelはrequestをまたいで効くが、HTTP/2 multiplexは主ワークロードであるsmall unary / small server streamingには直接効きにくい。
- spikeはmain比でwarm unary `+13.3%`、server streaming count=1000 `+6.3%` の退行があり、主ワークロードを遅くしてまで入れる根拠が弱い。
- async runtime、Fiber、明示的な並列RPC API、transport専用threadを導入する段階では価値があるが、現時点のrelease default gateには不要。

## 今後戻る条件

以下のいずれかを満たす段階で再検討する。

- public APIとしてasync / concurrent RPCを扱う。
- 同一request内で複数server streamingを同時に進める実ユースが出る。
- transport専用thread / shared event loopを導入し、PHP main threadとtransport progressを分離する。
- 単一active stream時のfast pathを維持し、small unary / small streamingの退行を `+3%` 程度以内に抑えられる。
