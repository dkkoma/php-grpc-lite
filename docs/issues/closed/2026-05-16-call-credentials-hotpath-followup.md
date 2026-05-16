---
Status: Closed
Owner: Codex
Created: 2026-05-16
Branch: main
---

# CallCredentials bridge固定費候補の扱い

## 目的

real Spanner mixed transactionで `grpc_lite_merge_call_credentials_metadata()` がCPU差の主因候補か確認する。

## 調査

- `Grpc\AbstractCall` は `call_credentials_callback` optionがある場合だけ `CallCredentials::createFromPlugin()` を作る。
- `google/cloud-spanner` / `google/gax` の通常経路は `BaseStub` の `update_metadata` と `CredentialsWrapper` によりmetadataを付与する。
- real Spanner Laravel/FPM fixtureでは、C拡張の `CallCredentials` callback経路を主に使っていない。

## 判断

- 採用しない。
- `grpc_lite_merge_call_credentials_metadata()` 自体には `strpprintf` / zval setup / PHP callback / hash mergeの固定費があるが、今回の実ワークロードCPU差の主因ではない。
- 将来 `call_credentials_callback` を多用するユースケースが出た場合に、別issueとしてservice URL cacheやcallback引数固定費を検証する。

## 検証

- コードリーディングのみ。実装変更なし。
