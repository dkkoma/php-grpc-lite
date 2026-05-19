---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - https://github.com/dkkoma/php-grpc-lite/pull/6
  - https://github.com/dkkoma/php-grpc-lite/issues/5
  - docs/reviews/issues/2026-05-20-pr6-http2-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-grpc-specialist-review.md
  - docs/reviews/issues/2026-05-20-pr6-spanner-specialist-review.md
---

# PR #6 active PING / BDP probe 検証方針の再整理

## 目的

PR #6 のBDP PING / SETTINGS関連変更を、merge前提の機能追加ではなく、GitHub issue #5 のSpanner遅延差を説明するための検証作業として再整理する。

特に、HTTP/2 / gRPC / Spanner専門レビューで指摘された以下を解消する。

- `grpc_lite.active_bdp_probe=1` default-on は検証結果に対して強すぎる。
- DATA受信後のclient-origin PINGは、同一RPCのfirst responseを直接速くするものではなく、後続RPCに効き得るconnection-level state変化として扱う必要がある。
- `active_bdp_probe_min_interval_ms=0` は長いserver streamingでRTT-rate PINGになり得る。
- ext-grpc 1.58.0 が `grpc.http2.bdp_probe=0` でも速い結果があり、BDPを根因とする推論は弱い。
- SETTINGS update実験は静的targetの負の実験であり、gRPC CoreのBDP estimator / flow-control parityではない。

## 背景

real Cloud Spanner `ExecuteStreamingSql SELECT 1` + service-account JSON 条件では、grpc-liteにresponse DATA callback起点のclient-origin PINGを入れるとp50/meanが大きく改善した。

一方、低RTT synthetic / emulator / high-level real-client benchでは、default-on active PINGにp99やp50の悪化が見えた。したがって、現時点の証拠は「Spanner issue #5で有効な可能性があるopt-in実験」を支持するが、「gRPC client全体のdefault behavior」を支持しない。

## gRPC Core v1.58.0 BDP re-arm model

ローカル `_research/grpc` の v1.58.0 系C-core確認結果:

- `BdpEstimator` の `inter_ping_delay_` 初期値は100ms。
- connection開始時は `bdp_ping_blocked=true` になり、最初のDATA frame受信で `schedule_bdp_ping_locked()` が呼ばれる。
- PINGがwireに乗った後 `StartPing()`、ACK完了で `CompletePing()` が呼ばれる。
- `CompletePing()` はincoming bytes / RTT / bandwidthからestimateを更新し、次回PING時刻を返す。
- estimateが伸びた場合は `inter_ping_delay_ /= 2`、安定時は100〜200msずつ増やし、最大10s方向へ伸ばす。
- 次回timer到達時にaccumulatorが0なら再度blockedに戻り、DATA受信までPINGしない。

したがって、grpc-lite PR #6の `min_interval_ms=0` はCoreのBDP estimatorとは異なる。Coreに寄せるなら、少なくともopt-in時の既定intervalは100ms相当が自然。ただし、issue #5の実測では1000msでは効果が薄く、0msが最も効いたため、0msは診断用overrideとして残す。

## スコープ

- `grpc_lite.active_bdp_probe` をdefault offに戻す。
- `grpc_lite.active_bdp_probe_min_interval_ms` の既定値をCore初期値に近い100msにする。
- positive PHPTは明示的に `active_bdp_probe=1` / `min_interval_ms=0` を指定する。
- disabled/no-op PHPTを追加し、default off時にclient-origin PINGとACK-triggered SETTINGS updateが出ないことを固定する。
- docsではBDP root causeと断定せず、active PINGがSpanner repeated streamの後続RPC connection stateに効く可能性として扱う。
- ext-grpc側のADC / service-account JSON / `grpc.http2.bdp_probe=0` matrixを再確認し、BDP以外のC-core差分を次の仮説として残す。

## 非スコープ

- gRPC Core BDP estimatorの完全実装。
- keepalive PINGの実装。
- Google Spanner専用auto enable。
- PR #6をそのままmergeすること。

## 計画

- [x] レビュー結果をPRブランチへコミットする。
- [x] gRPC Core v1.58.0 のBDP re-arm / intervalを確認する。
- [x] default off + 100ms interval defaultへ変更する。
- [x] disabled/no-op PHPTを追加する。
- [ ] docsの推論表現を修正する。
- [x] PHPT / C unit / static analysisを通す。
- [ ] real Cloud Spanner `SELECT 1` の必要最小matrixを再計測または既存結果に基づいて整理する。
- [ ] PR #6の説明をmerge候補ではなく検証ブランチとして更新する。

## 判断ログ

- 2026-05-20: 3ロールレビューにより、default-on active PINGはBlockerと判断した。
- 2026-05-20: Core BDP estimatorの初期re-arm delayは100msであり、PR #6の0ms defaultはCore parityではないと判断した。
- 2026-05-20: ただしSpanner issue #5では0msが最も効いたため、0msを診断overrideとして残す。
- 2026-05-20: `active_bdp_probe` をdefault offに戻し、`active_bdp_probe_min_interval_ms` の既定値を100msに変更した。positive trace testは明示opt-inにし、disabled/no-op PHPTを追加した。

## 検証

- `./tools/test/check-phpt.sh`: PASS, 17/17
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
