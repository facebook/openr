---
oncalls: ['routing_protocol']
apply_to_path: 'fbcode/openr/.*'
apply_to_regex: '.*\.(h|hh|hpp|hxx|c|cc|cpp|cxx|c\+\+|tcc)$'
description: C++ coding standards for Open/R.
---

# C++ Rules

> This rule extends `fbcode/.llms/rules/fbcode-cpp.md` with Open/R-specific overrides and additions; general fbcode C++ rules from there still apply.

## BUCK Targets
- MUST have **one source file per build target**. Each `X.h`/`X.cpp` pair gets its own `cpp_library`. Do NOT bundle multiple source files into a single giant library.
- MUST NEVER use `glob()` in TARGETS files.
- MUST use autodeps: `arc lint --take AUTODEPS --apply-patches`.

```python
# GOOD — one file per target, clear dependency graph
cpp_library(
    name = "KvStore",
    srcs = ["KvStore.cpp"],
    headers = ["KvStore.h"],
)

# BAD — giant monolithic library causes circular dependencies
cpp_library(
    name = "openr_lib",
    srcs = glob(["*.cpp"]),
    headers = glob(["*.h"]),
)
```

## File Organization & Dependencies
- Use `#pragma once` for header guards.
- Include order: (1) related header, (2) C++ std, (3) third-party (folly, thrift), (4) Meta internal, (5) project headers.
- NEVER include headers in `.h` files that are only needed in `.cpp` files.
- NEVER use `using namespace` in headers. Use specific using-declarations in `.cpp` only.
- Only include minimum required headers — avoid heavyweight umbrella headers.
- Be cautious with `//common/*` deps — they often pull large transitive trees. Prefer folly equivalents.

## Memory & Safety
- Avoid singletons — they cause Static Initialization Order Fiasco.
- Use `folly::not_null` or `FOLLY_NONNULLABLE`/`FOLLY_NULLABLE` for pointer safety.

## Data Structures & Containers
- Prefer `folly::F14FastMap`/`F14FastSet` over `std::unordered_map`/`std::unordered_set`.

## Error Handling
- Use `fmt::format` for error messages with context.
- Use `folly::Try<T>` for operations that may fail.
- Throw `std::logic_error` for programming errors (broken invariants).
- Create custom exceptions inheriting `std::runtime_error` for domain errors.
- Propagate errors to callers in library code — don't catch and log internally.
- Handle errors exactly once: either return error to caller OR handle it, never both.

## Performance
- Avoid creating `shared_ptr` in hot per-element processing loops. Prefer `unique_ptr` or pass existing `shared_ptr` by `const&`.

## Logging

### PROHIBITED: LOG, VLOG, and glog macros
NEVER use `LOG()`, `VLOG()`, or any glog macro. These are synchronous and block the calling thread on `write()` system calls during log rotation, which can cause thread stalls.

### Required: Use XLOG family
```cpp
// PROHIBITED — blocks the calling thread synchronously
VLOG(1) << "state changed";
LOG(INFO) << "session established";

// BAD — fmt::format evaluated eagerly even if log level disabled
XLOG(INFO) << fmt::format("state changed to {}", state);

// ACCEPTABLE — stream-based, matches existing openr style
XLOG(INFO) << "state changed to " << state;

// PREFERRED for new code — formatting deferred, only runs if log level enabled
XLOGF(INFO, "state changed to {}", state);
```
- SHOULD prefer `XLOGF` over stream-style `XLOG << ...` in new code. Existing stream-style code is not in scope for opportunistic rewrites — migrate only when touching the line for other reasons.
- Use `XLOG_EVERY_MS(severity, ms)` or `XLOG_EVERY_N(severity, count)` for high-frequency log paths.
- Use `XLOG_IF(severity, condition)` for conditional logging.

## Comment Style
- MUST use block comment format for multi-line comments:
```cpp
/*
 * Explanation of why this logic exists
 * and any non-obvious constraints.
 */
```
- Do NOT use `//` style for multi-line comments spanning more than one line.

## Common Pitfalls

### Callback memory leak via `shared_from_this`
```cpp
// BAD — captured shared_ptr prevents cleanup
auto cb = [self = shared_from_this()]() { self->process(); };

// GOOD — weak_ptr breaks the cycle
auto cb = [weak = weak_from_this()]() {
  if (auto self = weak.lock()) { self->process(); }
};
```

## Testing (C++ specific)
- Use GTest with **Arrange-Act-Assert**.
- Use `ASSERT_*` for preconditions, `EXPECT_*` for value checks.
- Use `folly::Baton` for async test synchronization (not sleep).
- Place mocks under `openr/tests/mocks/` (e.g., `MockIoProvider`, `MockNetlinkFibHandler`).
- Run with ThreadSanitizer: `buck2 test @mode/dev-tsan fbcode//path:test`.
