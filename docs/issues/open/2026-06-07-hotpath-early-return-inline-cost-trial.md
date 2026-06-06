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

## 検証

- 未実施。

## 判断ログ

- 2026-06-07: `ZEND_VM_KIND_TAILCALL` はZend VM executor dispatchの話であり、php-grpc-lite拡張Cコードの直接最適化とは別レイヤーと整理した。
- 2026-06-07: `always_inline` ではなく、まずearly return / slow path splitで通常optimizerが読みやすい形になるかを見る。

## 完了条件

- 対象候補、before/after、採用/棄却判断を記録する。
- 採用する場合は検証結果と副作用を記録する。
- 採用しない場合も、なぜ見送るかを記録してissueを閉じる。
