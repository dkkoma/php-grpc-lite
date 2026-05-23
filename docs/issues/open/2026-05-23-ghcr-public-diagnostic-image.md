# GHCR public diagnostic image

Status: Open

## 目的

GCP上の小さなVMから同一の診断containerをpullして実行できるように、Spanner / Pub/Sub / Secret Manager repro用imageをGHCRへ公開する。

## 背景

real SpannerのHTTP/2 wire shape / RTT差分はローカルネットワークの揺れを受けやすい。GCP近傍で再現性のある比較をするには、ローカルでbuildせず、GitHub Actionsでbuild済みimageをpullして実行する形がよい。

## スコープ

- `tools/diagnostics/issue5-spanner-repro` をGHCRへpublishするworkflowを追加する。
- official ext-grpc / php-grpc-lite のvariantを別tagでpushする。
- GCE等からpullして実行するためのREADME手順を追加する。

## 非スコープ

- image内でベンチ結果を永続保存する仕組み。
- GCP VM作成やIAM設定の自動化。
- production runtime imageの配布設計。

## 計画

- GitHub Actions workflowを追加し、manual dispatchとmain pushでGHCRへpushする。
- image tagはvariantとgit shaを含めて追跡可能にする。
- package visibilityは初回push後にGitHub package設定でpublic化する。

## 進捗

- 2026-05-23: issue作成。
- 2026-05-23: `publish-diagnostic-images.yml` を追加。`official` / `lite` の2 tagをGHCRへpushする。
- 2026-05-23: repro Dockerfileを `BENCH_SCRIPT` envでscript選択できる形に変更し、1 variant 1 imageで複数診断scriptを実行できるようにした。

## 検証

- 2026-05-23: `git diff --check` OK。
- 2026-05-23: `docker buildx build --check tools/diagnostics/issue5-spanner-repro` OK。

## 判断ログ

- GHCRを使う。Artifact RegistryはGCP IAM面では自然だが、今回の用途は公開診断imageのpullであり、GitHub ActionsからpushしやすいGHCRを優先する。
- GHCR package visibilityは初回publish後にGitHub UIでpublicへ変更する。GitHub Actionsの`packages: write`はpush権限であり、visibility変更の自動化は今回のスコープに含めない。

## 完了条件

- GHCR publish workflowが追加されている。
- pull/run手順がドキュメント化されている。
- 初回push後にpackageをpublicへ変更できる手順が明記されている。
