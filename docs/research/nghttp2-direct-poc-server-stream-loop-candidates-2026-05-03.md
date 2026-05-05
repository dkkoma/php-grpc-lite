# nghttp2 direct PoC: server streaming receive loop candidates (2026-05-03)

## 目的

server streaming large response の残差候補を、1つずつ実装ノブ化して比較する。

前回までに、body append / decode / `nghttp2_session_mem_recv()` 自体は主因ではなく、receive-side flow-control と read progression が支配的であることが見えていた。今回は残候補を個別に試した。

## 固定条件

- nghttp2 PoC
- server streaming
- response body discard
- no-copy request path
- poll loop
- client receive stream window: 16MiB
- client receive connection window: 16MiB
- Go test-server: default server options

比較ケース:

- 10 messages × 100KiB
- 1 message × 1MiB

## 候補

| candidate | 内容 | 期待 |
| --- | --- | --- |
| `recv64` | socket receive buffer を 64KiB に拡大 | syscall/poll回数低減 |
| `recv256` | socket receive buffer を 256KiB に拡大 | syscall/poll回数低減 |
| `flush` | `nghttp2_session_mem_recv()` 後に `nghttp2_session_send()` を即実行 | WINDOW_UPDATE / ACK を早く返す |
| `readfirst` | poll loop の先頭で read を先に drain | readiness を逃さず response DATA を先に進める |

## 単独比較

### 10 messages × 100KiB

| candidate | p50 | p99 | poll wait p99 | recv p99 | mem_recv p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | 571.0μs | 3866.0μs | 3803.0μs | 129.0μs | 15.0μs |
| recv64 | 481.0μs | 3653.0μs | 3563.0μs | 128.0μs | 13.0μs |
| recv256 | 516.0μs | 3372.0μs | 3286.0μs | 132.0μs | 13.0μs |
| flush | 532.0μs | 3697.0μs | 3634.0μs | 118.0μs | 20.0μs |
| readfirst | 511.0μs | 4020.0μs | 3952.0μs | 101.0μs | 13.0μs |

### 1 message × 1MiB

| candidate | p50 | p99 | poll wait p99 | recv p99 | mem_recv p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | 477.0μs | 4493.0μs | 4264.0μs | 112.0μs | 25.0μs |
| recv64 | 475.0μs | 4610.0μs | 4529.0μs | 92.0μs | 11.0μs |
| recv256 | 476.0μs | 4070.0μs | 3997.0μs | 118.0μs | 12.0μs |
| flush | 453.0μs | 4325.0μs | 4219.0μs | 89.0μs | 12.0μs |
| readfirst | 484.0μs | 4221.0μs | 4160.0μs | 139.0μs | 35.0μs |

判断:

- `recv256` は 10×100KiB / 1×1MiB の両方で p99 改善があり、採用候補。
- `flush` は単独でも改善するが、`recv256` より弱い。
- `readfirst` は p50 を下げるケースがあるが p99 が不安定で、採用候補から外す。
- `recv64` は 10×100KiB では改善するが、1×1MiB では悪化した。中途半端な buffer size は採らない。

## 組み合わせ比較

| candidate | case | p50 | p99 | poll wait p99 | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| recv256 + flush | 10×100KiB | 515.0μs | 3447.0μs | 3243.0μs | 3178.4μs |
| recv256 + flush | 1×1MiB | 437.0μs | 3502.0μs | 3424.0μs | 3080.7μs |
| recv256 + readfirst | 10×100KiB | 408.0μs | 3828.0μs | 3744.0μs | 3431.3μs |
| recv256 + readfirst | 1×1MiB | 421.0μs | 4172.0μs | 4084.0μs | 3842.7μs |
| recv256 + flush + readfirst | 10×100KiB | 418.0μs | 3813.0μs | 3731.0μs | 3329.2μs |
| recv256 + flush + readfirst | 1×1MiB | 430.0μs | 3866.0μs | 3755.0μs | 3246.9μs |

判断:

- 最良は `recv256 + flush`。
- `readfirst` は組み合わせても p99 を悪化させるため、採らない。
- `recv256 + flush` でも `poll wait p99` が total p99 の大半を占めるが、server `OutPayload` p99 も同時に下がるため、client receive progression が server send progress に効いていると見る。

## ext-grpc / php-grpc-lite との比較

通常 server 条件で再取得した比較。

| case | implementation | throughput | p50 | p99 | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| 10×100KiB | nghttp2 PoC best | 12003.4 msg/s | 515.0μs | 3447.0μs | 3178.4μs |
| 10×100KiB | ext-grpc | 11736.8 msg/s | 618.6μs | 3243.7μs | 3054.0μs |
| 10×100KiB | php-grpc-lite | 7819.0 msg/s | 952.4μs | 4477.1μs | 3283.0μs |
| 1×1MiB | nghttp2 PoC best | 1409.7 msg/s | 437.0μs | 3502.0μs | 3080.7μs |
| 1×1MiB | ext-grpc | 1250.3 msg/s | 569.2μs | 3736.6μs | 2989.4μs |
| 1×1MiB | php-grpc-lite | 786.6 msg/s | 945.7μs | 5236.1μs | 4592.0μs |

## 結論

試した候補のうち、HTTP/2 transport に入れる価値があるのは以下。

1. receive stream / connection window を大きくする。
2. receive buffer を 256KiB 程度に広げる。
3. `nghttp2_session_mem_recv()` 後に、pending WINDOW_UPDATE / ACK を即 flush する。

採らない候補:

- read-first poll loop。p50 は下がることがあるが p99 が悪化しやすい。
- 64KiB receive buffer。効果がケース依存で、1MiB response では悪化した。

best PoC は 1×1MiB では ext-grpc の p99 を下回り、10×100KiB では ext-grpc に約 200μs 届かない程度まで来ている。残差は client内CPUではなく、server `OutPayload` 進行と poll wait に残る。
