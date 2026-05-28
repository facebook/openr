---
oncalls: ['routing_protocol']
apply_to_path: 'fbcode/openr/.*'
description: General rules for Open/R code authoring and review.
---

# General Rules

## Thrift
- MUST place thrift files under `if/` subdirectory. Use `PascalCase.thrift` naming (e.g., `KvStore.thrift`, `OpenrConfig.thrift`).
- MUST use PascalCase for structs/enums/services, lowerCamelCase for methods, UPPER_SNAKE_CASE for enum values.
- MUST NEVER mark fields as `required`. NEVER use `thrift::FRAGILE` constructors.
- MUST use `field() = value` for assignment, NEVER `*field_ref()` or `*obj.field()`.
- MUST NOT remove deprecated thrift fields until all production consumers have migrated.
- SHOULD avoid `optional` unless serialization cost matters. SHOULD NOT nest optionals.
- SHOULD wrap API inputs/outputs in separate Thrift structs for extensibility.
- SHOULD terminate field/method definitions with `;`, enum values with `,`.
- SHOULD place shared thrift structs in common libraries to prevent duplication.

## Comments & Documentation
- MUST provide examples in comments to remove ambiguity about format or type.
- MUST annotate boolean params at call sites: `/*isInitialSync=*/true` (C++) or `is_initial_sync=True` (Python).

## Testing
- MUST cover error handling paths.
- MUST fix TSAN failures by restructuring the test, not adding locks to production code.
- MUST run stress tests (100+ iterations, TSAN mode) to validate flaky test fixes.
- MUST never use sleep-based synchronization — use batons, futures, or queue-driven signaling.
- MUST use dynamic ports (`port=0`) to prevent port conflict flakes.
- MUST write a dedicated UT to reproduce production bugs before fixing — keep as regression guard.
- MUST test boundary/edge cases: float precision, invalid inputs, off-by-one timing.
- MUST NOT remove test coverage without understanding root cause and providing replacement.
- SHOULD use `EXPECT_EQ(expected, actual)` ordering consistently.
- SHOULD use `UnorderedElementsAre` instead of hardcoded vector ordering.
- SHOULD use parameterized tests (`TEST_P`) instead of helper-function repetition.

## Migration & Feature Gating
- MUST guard all new features with a config flag (disabled by default) for instant rollback.
- MUST keep both old and new code paths working and tested during migrations.
- SHOULD remove deprecated code aggressively once migration is verified in production.

## Diff Authorship
- MUST use bracket tags in title: `[openr][feature]: <description>` (e.g., `[openr][kvstore]: Pre-compress flood-pub payload`).
- MUST provide **why** (motivation) and **what** (details) in summary. Include links to docs/tasks/SEVs.
- MUST make each diff atomic and self-contained:
  - NEVER break tests in one diff and fix in another.
  - ALWAYS include tests alongside code in the same diff.
  - More than **500 significant lines** is hard to review — split into focused pieces.
  - Do NOT introduce functions over **200 lines**. One function = one thing.
- MUST include buck2 test commands with expected output in test plan. Provide evidence, not vague statements.
