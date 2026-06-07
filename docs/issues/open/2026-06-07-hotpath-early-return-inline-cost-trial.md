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
- 2026-06-07: 同条件before/afterで実測改善が見えなかったため、trial実装は採用せず `src/transport.c` から戻した。診断環境と調査記録は残す。

## 観測

### 同条件 before/after: Clang/Bloaty

`df5bce2` をbefore、`f22189e` をafterとして、同じ `dev-optimizer` 環境でDSO reportを取り直した。

| item | before | after | 判断 |
| --- | ---: | ---: | --- |
| commit | `df5bce2` | `f22189e` | 同じ診断環境 |
| clang | 19.1.7 | 19.1.7 | 同条件 |
| LLD | 19.1.7 | 19.1.7 | 同条件 |
| Bloaty | 1.1 | 1.1 | 同条件 |
| grpc.so bytes | 528,752 | 531,056 | debug情報込みで +2,304 bytes |
| `.text` | 53.6 KiB | 53.6 KiB | 実質不変 |
| clang inline success | 941 | 926 | 全体数では改善なし |
| clang inline missed | 171 | 167 | missed数は少し減ったが全体判断には弱い |

対象callsite:

| callsite | before | after | 判断 |
| --- | --- | --- | --- |
| `append_request_header` -> `append_custom_request_header_value` | not inlined, cost 65 / threshold 45 | `append_request_header_unchecked` が inlined, cost -5 / threshold 45 | 局所splitは成功 |
| `append_custom_request_header_value` -> `append_custom_request_headers` | not inlined, cost 1105 / threshold 225 | not inlined, cost 1025 / threshold 225 | costは下がったがthresholdには遠い |
| `is_binary_metadata_header` in `append_custom_request_header_value` | 2回評価 | 1回評価 | 局所的な重複評価は解消 |

Bloaty symbols上位はbefore/afterとも `grpc_lite_unary_call_perform_on_connection`, `send_callback`, `create_h2_connection`, `server_streaming_call_open_resource`, `zim_Call_startBatch` で、今回の小変更がDSO上の大きなsymbol shape変更を起こしている様子はない。

### 同条件 before/after: bench

通常dev buildへ戻した後、before/afterでmetadata系benchを取った。baseline worktreeは `COMPOSE_PROJECT_NAME=php-grpc-lite` で既存compose project/networkを共有し、`vendor/` は同じ依存を一時コピーした。

`metadata-header --calls=200`:

| measurement | before p50 us | after p50 us | before p99 us | after p99 us | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| req_0_resp_0_value_0b | 35.5 | 35.9 | 219.4 | 1116.7 | metadataなし、対象外ノイズ |
| req_10_resp_0_value_32b | 35.5 | 31.9 | 521.0 | 608.7 | p50は改善方向、p99は悪化方向 |
| req_10_resp_10_value_32b | 48.1 | 46.9 | 649.7 | 398.1 | 改善方向だがlatency系なので揺れあり |
| req_50_resp_0_value_32b | 101.9 | 103.6 | 1399.8 | 1185.8 | ほぼ不変 |
| req_50_resp_50_value_32b | 151.7 | 153.2 | 1565.2 | 696.7 | p50はほぼ不変、p99は改善方向 |

`cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3` の metadata case:

| measurement | before cpu_us/call | after cpu_us/call | 判断 |
| --- | ---: | ---: | --- |
| metadata_unary_req10_resp10_32b repeat 1 | 14.6 | 14.6 | 同等 |
| metadata_unary_req10_resp10_32b repeat 2 | 14.6 | 14.8 | 微悪化 |
| metadata_unary_req10_resp10_32b repeat 3 | 14.3 | 15.3 | 悪化方向 |
| average | 14.5 | 14.9 | 改善なし、ノイズ込みで微悪化寄り |

bench上は、metadata-heavy条件でも採用根拠になる改善は見えない。`metadata-header` のp99は揺れており、CPU summaryを優先すると「局所remarksは改善したが、実測性能改善としては不採用」が妥当。

## 検証

- `docker compose config --services`: `dev-optimizer` serviceを確認。
- `docker compose build dev-optimizer`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。
- `./tools/test/check-c-static-analysis.sh`: PASS。
- `./tools/test/check-phpt.sh`: PASS。15/15。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked`: PASS。
- `./bench/run.sh metadata-header --calls=30`: PASS。afterのみ。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/before-header-unchecked`: PASS。baseline `df5bce2`。
- `tools/diagnostic/optimizer-dso-report.sh var/optimizer-dso-report/after-header-unchecked-v2`: PASS。after `f22189e`。
- `./bench/run.sh metadata-header --calls=200`: PASS。before run id `hotpath-before-metadata`, after run id `hotpath-after-metadata`。
- `./bench/run.sh cpu-micro --calls=2000 --warmup-calls=100 --repeat-runs=3`: PASS。before run id `hotpath-before-cpu`, after run id `hotpath-after-cpu`。

## 判断ログ

- 2026-06-07: `ZEND_VM_KIND_TAILCALL` はZend VM executor dispatchの話であり、php-grpc-lite拡張Cコードの直接最適化とは別レイヤーと整理した。
- 2026-06-07: `always_inline` ではなく、まずearly return / slow path splitで通常optimizerが読みやすい形になるかを見る。
- 2026-06-07: 今回のunchecked append splitはremarks上の局所costを下げるが、関数全体のinline可否は変わらない。benchもafter単独なので、この時点では採用確定ではなくtrial結果として扱う。
- 2026-06-07: 同条件before/afterを取った結果、局所的なinline改善は確認できたが、DSO `.text` とmetadata CPU benchでは採用根拠になる改善は見えなかった。この変更自体は性能改善としては不採用寄り。調査を広げるなら、metadata pathよりもSpanner形状で支配的なserver streaming / response processing / persistent connection preflight周辺を優先する。
- 2026-06-07: 不採用判断に合わせてtrial実装は戻す。PRには診断環境追加と、remarksを使った小さいsplit実験の結果を残す。

## 完了条件

- 対象候補、before/after、採用/棄却判断を記録する。
- 採用する場合は検証結果と副作用を記録する。
- 採用しない場合も、なぜ見送るかを記録してissueを閉じる。
