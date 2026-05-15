---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# transport loop呼び出し回数の改善余地を検証する

## 目的

`poll` / `SSL_read` / `SSL_write` / `nghttp2_session_send` / `nghttp2_session_mem_recv` の呼び出し粒度が細かすぎてCPU固定費になっていないか確認する。

## 背景

別スレッド化よりも、同期transport loop内の呼び出し回数やread/write粒度のほうがCPU差の説明になりやすい。現状のstraceではsyscall爆発は見えていないが、OpenSSL/nghttp2内部呼び出し回数はまだ直接観測していない。

## スコープ

- 既存実装のloop粒度をコード上で確認する。
- 追加counterまたは既存bench診断で呼び出し回数を確認する。
- 明確な無駄があれば修正する。

## 非スコープ

- dedicated transport thread PoC。
- HTTP/2 multiplex設計変更。

## 計画

1. 既存bench診断counterで観測可能な項目を確認する。
2. 不足があればbench build限定counterを追加する。
3. 改善余地がある場合のみproduction codeを修正する。

## 進捗

- mixed transaction経路でstraceを確認した。
- deadline socket timeout hot path修正後、`setsockopt` の過多は再発していない。
- transport loop自体のsyscall頻度は、現時点のCPU差を説明する主因ではない。

## 検証

- `native-strace-mixed-20260515`:

| syscall | calls / 10 iterations | note |
| --- | ---: | --- |
| `setsockopt` | 4 | deadline I/O hot pathは解消済み |
| `sendto` | 150 | mixed transactionのRPC数相当 |
| `recvfrom` | 221 | 141 errors含む |
| `ppoll` | 81 | network wait中心 |

- syscall total timeは約0.010643sで、mixed transaction全体wall timeの支配要因ではない。

## 判断

- 現時点ではproduction codeを修正しない。
- OpenSSL/nghttp2内部呼び出し回数まで見るにはsampling profilerが必要だが、controlled CPU microではnative transport構造に大きなCPU hot pathは見えていない。
- 追加counterをproduction codeへ入れるとノイズと複雑性が増えるため採用しない。

## 完了条件

- loop粒度がCPU差の主要因かどうか判断できる。

## 完了

- syscall粒度では主因ではないと判断したためClosed。
- 修正コミット: this commit
