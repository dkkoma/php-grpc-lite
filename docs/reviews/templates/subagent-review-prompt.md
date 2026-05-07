# Subagent Review Prompt Template

```text
Review the current repository state as <review role>.

Scope:
- <files/directories>

Read the minimum necessary local context:
- AGENTS.md
- docs/reviews/README.md
- docs/reviews/templates/review-issue.md
- relevant design/code-reading/spec docs
- changed files and adjacent tests

Do not act as a search engine.

Write review findings directly to:
- `docs/reviews/issues/<YYYY-MM-DD>-<topic>-<agent-name-or-role>.md`

Use `docs/reviews/templates/review-issue.md` as the format. Each review agent must write to its own file. Do not share or co-edit one issue file with other agents. If your own file already exists for this review topic, update it instead of creating a duplicate.

Do not return chat-only findings after a successful write. If you cannot write files, return the issue-formatted findings and explicitly say: `parent agent must persist this review`.

Focus on:
- <role-specific review points>

For each finding, include:
- Severity: Blocker / High / Medium / Low / Design Decision
- Status: Open
- Reviewer role
- Finding
- Evidence
- Expected model
- Why it matters
- Recommended fix

Use `none` for empty severity categories.

Return:
- the review issue file path
- a concise summary of finding counts
```
