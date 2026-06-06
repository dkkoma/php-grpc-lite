---
Status: Open
Owner: Codex
Created: 2026-06-07
Branch: codex/hotpath-early-return-trial
---

# hotpath early return / slow path splitでinline costを下げられるか確認する

## 目的

Clang optimization remarksでinline cost / thresholdが見えるようになったため、C拡張のhot pathで early return と rare/error path の分離が、可読性とoptimizer判断の両方に効くかを小さく検証する。

性能改善の採用ではなく、まず「どの関数で cost が下がるか」「DSO上の `.text` / symbol shape がどう変わるか」「controlled benchで悪化しないか」を確認する。

## 背景

`docs/issues/closed/2026-06-06-release-artifact-lto-dso-investigation.md` で、Clang ThinLTOのoptimization remarksから以下のような判断情報を読めることを確認した。

- `h2_connection_send` は caller へは cost 1560 / threshold 225 でinlineされない。
- `configure_tls_connection` は `create_h2_connection` へ cost 2555 / threshold 225 でinlineされない。
- 小さいhelperは cost model によりinlineされる一方、I/O、trace、TLS、error detail処理を含む関数はtoo costlyになりやすい。

この結果から、単純に `always_inline` を付けるのではなく、まずhot pathの読みやすさを損なわない範囲で early return と slow path split を試す。

## 仮説

- hot path内に混在しているrare/error pathを early return または cold helperへ分けると、正常系の直線的な範囲が読みやすくなる。
- Clang remarks上で対象callsiteの cost が下がる、または missed inline の理由が変わる可能性がある。
- ただしI/Oやnghttp2 callbackが支配的な関数では、`.text` やbenchに見える差は小さい可能性が高い。

## スコープ

- Clang optimization remarksから、hot path候補の missed inline と cost / threshold を確認する。
- `transport.c` 周辺の小さい候補に限定し、early return / slow path split を1〜2箇所だけ試す。
- before/afterで以下を比較する。
  - Clang optimization remarks
  - Bloaty / `size -A` / symbol diff
  - controlled CPU micro bench
  - PHPT / C unit / static analysis

## 非スコープ

- `always_inline` の大量付与。
- `-O3` / LTO / PGO のdefault採用。
- transport lifecycleやHTTP/2/gRPC semanticsの変更。
- 大規模な `transport.c` 分割や責務変更。
- ext-grpcの数値に近づけること自体を目的にすること。

## 計画

1. 現在の `main` 起点 + LTO DSO調査issue取り込み状態で、Clang remarksの対象候補を再確認する。
2. 候補を1〜2箇所に絞り、beforeのremarks / DSO / controlled benchを記録する。
3. early returnまたはslow path splitを小さく実装する。
4. afterのremarks / DSO / controlled benchを同条件で取り、改善・悪化・ノイズを分類する。
5. 効果が小さい、または複雑性が増える場合は採用せず、調査結果として閉じる。

## 採否基準

- 採用候補:
  - hot pathの可読性が上がる。
  - PHPT / C unit / static analysis が通る。
  - controlled benchで悪化しない。
  - remarksまたはDSO上で、対象関数のcost低下、symbol整理、`.text`縮小などの説明可能な差分がある。
- 棄却候補:
  - cost / DSO / benchに意味のある差がない。
  - cold helper化で責務やlifecycleが読みにくくなる。
  - error handlingやconnection lifecycleの見通しが悪くなる。

## 進捗

- 2026-06-07: issue作成。
- 2026-06-07: 作業ブランチを `main` 起点で作成し、前回のLTO DSO調査issueを取り込んだ。
- 2026-06-07: Clang/Bloaty診断を毎回手作業で組まなくてよいように、`dev-optimizer` compose service と `tools/diagnostic/optimizer-dso-report.sh` を追加した。
- 2026-06-07: request metadata header append pathで、capacity確認済みのappendを `append_request_header_unchecked()` として分離し、custom metadata pathではgrow済み確認後にunchecked appendを呼ぶ形を試した。

## 観測

### before

前回のClang 22 ThinLTO remarksでは、custom metadata pathで以下が見えていた。

- `append_request_header` は `append_custom_request_header_value` へinlineされない: cost 65 / threshold 45。
- `append_custom_request_header_value` は `append_custom_request_headers` へinlineされない: cost 1105 / threshold 225。
- `is_binary_metadata_header` は同じ関数内で複数回評価されていた。

これは既存の `var/lto-dso-followup/clang22-thinlto/make.log` 由来で、今回追加した `dev-optimizer` と同一toolchainではない。同条件のbefore DSO/benchは未取得。

### after

`tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked`:

- Toolchain: Debian clang 19.1.7 + LLD 19.1.7 + Bloaty 1.1。
- `grpc_so_bytes=531056`。
- `.text`: 53.6 KiB。
- Clang inline remarks: success 940 / missed 175。
- `append_request_header_unchecked` は `append_custom_request_header_value` へinlineされた: cost -5 / threshold 45。
- `append_request_header_unchecked` は `append_request_header` へinlineされた: cost -5 / threshold 487。
- `append_custom_request_header_value` は引き続き `append_custom_request_headers` へinlineされない: cost 1025 / threshold 225。
- `is_binary_metadata_header` は `append_custom_request_header_value` 内で1回だけ評価される形になった。
- Bloaty symbols上位は `grpc_lite_unary_call_perform_on_connection`, `send_callback`, `create_h2_connection`, `server_streaming_call_open_resource`, `zim_Call_startBatch` で、今回の小変更が大きなsymbol shape変更を起こしている様子はない。

### bench

通常dev buildへ戻した後、metadata/header sweepを短めに実行した。

`BENCH_TAG=early-return-after BENCH_OTEL_RUN_ID=early-return-after BENCH_IMPLEMENTATION=php-grpc-lite ./bench/run.sh metadata-header --calls=30`

| measurement | count | span_p50_us | span_p99_us |
| --- | ---: | ---: | ---: |
| req_0_resp_0_value_0b | 30 | 50.8 | 4801.0 |
| req_10_resp_0_value_32b | 30 | 55.6 | 349.6 |
| req_10_resp_10_value_32b | 30 | 39.8 | 377.8 |
| req_50_resp_0_value_32b | 30 | 86.5 | 367.7 |
| req_50_resp_50_value_32b | 30 | 166.2 | 1411.3 |

afterのみの短時間runなので、性能改善の根拠にはしない。採否判断には同条件before/afterを別途取る。

## 検証

- `docker compose config --services`: `dev-optimizer` serviceを確認。
- `docker compose build dev-optimizer`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。
- `./tools/test/check-c-static-analysis.sh`: PASS。
- `./tools/test/check-phpt.sh`: PASS。15/15。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked`: PASS。
- `./bench/run.sh metadata-header --calls=30`: PASS。afterのみ。

## 判断ログ

- 2026-06-07: `ZEND_VM_KIND_TAILCALL` はZend VM executor dispatchの話であり、php-grpc-lite拡張Cコードの直接最適化とは別レイヤーと整理した。
- 2026-06-07: `always_inline` ではなく、まずearly return / slow path splitで通常optimizerが読みやすい形になるかを見る。
- 2026-06-07: 今回のunchecked append splitはremarks上の局所costを下げるが、関数全体のinline可否は変わらない。benchもafter単独なので、この時点では採用確定ではなくtrial結果として扱う。

## 完了条件

- 対象候補、before/after、採用/棄却判断を記録する。
- 採用する場合は検証結果と副作用を記録する。
- 採用しない場合も、なぜ見送るかを記録してissueを閉じる。
