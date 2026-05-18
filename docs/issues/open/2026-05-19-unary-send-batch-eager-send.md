---
Status: Open
Owner: Codex
Created: 2026-05-19
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# unary SEND batchを公式ext-grpcと同じくeager sendへ寄せる

## 目的

`Grpc\UnaryCall::start()` / `Grpc\Call::startBatch()` のSEND opsで、公式ext-grpcと同じくnetwork sendを開始する。現状のphp-grpc-liteはSEND opsを保存し、RECV_STATUS batchまたは`wait()`時にunary RPC全体を実行している。

## 背景

GitHub issue #5のmethod-level marker調査で、次の構造差が確認された。

- ext-grpc: `_simpleRequest()`内の`UnaryCall::start()`でSEND batchを実行し、その時点でnetwork sendが発生する。
- php-grpc-lite: `UnaryCall::start()`ではmetadata/payloadを保存するだけで、`UnaryCall::wait()`のRECV batchで初めてrequestを送信する。

GAXのPromiseが即時waitされる典型経路では総wall time差としては小さい可能性がある。ただし、これはAPI lifecycle互換性として弱く、`start()`後に別処理を挟む利用ではrequest開始時刻が公式ext-grpcと変わる。

## スコープ

- unary callのみ対象にする。
- SEND_INITIAL_METADATA / SEND_MESSAGE / SEND_CLOSE_FROM_CLIENT が揃った時点でHTTP/2 streamをsubmit/sendする設計を検討する。
- response受信、metadata/status構築、`wait()` APIは既存surfaceを維持する。
- persistent connection / deadline / call credentials / cancellation / error mappingを壊さない。

## 非スコープ

- client streaming / bidi streaming。
- ext-grpc C-core実装の模倣。
- issue #5のCommit poll wait差の主因と決め打ちすること。

## 計画

1. 現状の`grpc_lite_call_obj` lifecycleを整理し、SEND batch後に保持すべきactive call stateを定義する。
2. `Grpc\Call::startBatch()`でSEND opsが揃った場合、unary request送信までを行い、response受信はRECV batchまで遅延できるか検証する。
3. `wait()` / `getMetadata()` / cancel / deadline exceeded / connection failureの状態遷移をPHPTで追加する。
4. real Spanner markerで `BEGIN→WAIT_BEGIN` / `WAIT_BEGIN→WAIT_END` / `BEGIN→WAIT_END` をext-grpcと比較する。
5. HTTP/2/gRPC domain model reviewを実施する。

## 完了条件

- `UnaryCall::start()`でnetwork sendが始まることをstraceまたはdiagnosticで確認できる。
- 既存PHPTが通る。
- `start()`後、`wait()`前にuser codeが挟まるケースでも公式ext-grpcに近いlifecycleになる。
- real Spanner Commit計測で、総wall timeとwait区間の解釈が公式ext-grpcと比較可能になる。
