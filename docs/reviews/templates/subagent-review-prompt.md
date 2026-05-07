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

Do not edit files. Do not act as a search engine. Return only review findings in the repository review issue format.

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
```
