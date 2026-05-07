# Review Records

サブエージェントレビューの指摘と対応履歴を残す場所です。

## 目的

- レビュー指摘をコミット可能なMarkdownとして残す。
- 修正時に、どの指摘へ何をしたのかを同じissueに追記する。
- design docには現在・未来の設計だけを残し、過渡的な指摘履歴を混ぜない。

## 保存場所

- レビューエージェント単位のissue: `docs/reviews/issues/YYYY-MM-DD-<topic>-<agent-name-or-role>.md`
- issueテンプレート: `docs/reviews/templates/review-issue.md`

## 運用

1. レビューエージェントを起動する前に、対象scope、review role、各エージェントのissueファイル名を決める。
2. レビューエージェントには、指摘を自分専用の `docs/reviews/issues/` issueファイルへ直接作成または更新するよう依頼する。
3. レビューエージェントは、書き込み完了後にissueファイルパスと件数サマリーだけを返す。チャット本文だけで指摘を返さない。
4. レビューエージェントがファイルを書けない場合だけ、親エージェントが返されたissue形式の指摘を `docs/reviews/issues/` へ転記する。
5. 修正時は該当issueの `Status`, `Fix summary`, `Fix commit`, `Verification` を埋める。
6. 再レビューで指摘が消えたら、`Status: Fixed` または `Status: Accepted` にする。

## 言語

- サブエージェントへのレビュー依頼は英語でよい。
- issue本文は日本語を基本にする。
- HTTP/2 / gRPC / PHP extension の仕様語は英語のまま使う。

## Status

- `Open`: 未対応。
- `Fixed`: コードまたはドキュメントで対応済み。
- `Accepted`: 設計判断として受け入れ、必要なドキュメントへ明記済み。
- `Rejected`: 誤検知またはこのリポジトリでは不適用。理由を必ず書く。
