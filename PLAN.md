# PLAN — sans-IO refactor

Companion to [GOAL.md](GOAL.md). Single source of truth for the sans-IO
refactor: per-phase scope, conventions locked in, outcomes recorded.

## Status

| Phase | Scope | Status |
|---|---|---|
| 1 | Server-side `RequestParser` | ✅ Complete (2026-05-23) |
| 2 | Server-side `ResponseSerializer` | ✅ Complete (2026-05-23) |
| 3 | Rewire `Server::process_request` over the new core | ⏳ Next |
| 4 | Client side — `ResponseParser` + `RequestSerializer` | Pending |
| 5 | WebSocket frame codec | Pending |
| 6 | Hand-rolled `epoll` example | Pending |

## Approach decisions (locked in)

| Decision | Choice | Rationale |
|---|---|---|
| Refactor style | **Parallel build, then rewire** | Lowest risk of mid-refactor breakage; brief duplication is acceptable. |
| Phase 1 scope | **Thin vertical slice — request parser only** | Validate the API on real edge cases before committing the rest of the codebase to it. Existing `Server` untouched. |
| Header layout | **Extract core into [httplib_core.h](httplib_core.h)** | `httplib.h` stays drop-in. Power users can include only the core (no sockets, no TLS, no threads). |
| C++ standard | **C++17 floor for [httplib_core.h](httplib_core.h)** | Gives us `std::variant`, `std::optional`, `std::string_view`. `httplib.h` stays C++11-compatible. |

`std::expected` would be nicer for parser error returns but requires C++23
(clang 18+, gcc 13+) — we use an inline tagged enum instead and revisit
when the rest of the library is willing to bump.

## Conventions established in phase 1 (load-bearing for all later phases)

| Convention | Value |
|---|---|
| Namespace | `httplib::core::` |
| File | All sans-IO code in [httplib_core.h](httplib_core.h) |
| Header storage | Ordered `std::vector<std::pair<std::string, std::string>>` — preserves insertion order, allows duplicates, no case-insensitive comparator |
| Error reporting | `enum class ParseError` / `enum class SerializeError` + a `Failed` event. No exceptions, no `std::expected` |
| State machine style | **Polled** — caller drives via `feed()` then `poll()` (parser) or `begin()/write_body()/end()` then `pending_bytes()/consume()` (serializer) |
| Lifetimes | Returned `string_view`s point into core-owned buffers; invalidated by the next push call |
| Dependencies | Standard library only. **No include of `httplib.h`** |
| Header-only | Inline definitions throughout |

## Hard invariant — regression guarantee

These files **must not change** for phases 1–2:

- `httplib.h` — zero diff from `master`
- `test/test.cc` — zero diff
- All `example/*.cc` — zero diff

Phase 3 is where the rewire lands and `httplib.h` finally gets touched.

---

## Phase 1 — `RequestParser` (✅ complete)

### What landed

| File | Change |
|---|---|
| [httplib_core.h](httplib_core.h) | **New**, ~831 lines. Header-only, C++17, standalone. |
| [test/test_sansio_parser.cc](test/test_sansio_parser.cc) | **New**, 27 tests. |
| [test/CMakeLists.txt](test/CMakeLists.txt) | Additive: new `test-sansio-parser` target. |

### API

```cpp
namespace httplib::core {

enum class ParseError {
    None, InvalidRequestLine, InvalidHTTPMethod, InvalidHTTPVersion,
    InvalidHeaders, ExceedHeaderMaxLength, ExceedHeaderMaxCount,
    ExceedUriMaxLength, ExceedMaxPayloadSize, InvalidChunkedEncoding,
    ContentLengthAndTransferEncoding,
};

struct Limits {
    std::size_t max_header_length  = 8192;
    std::size_t max_header_count   = 100;
    std::size_t max_uri_length     = 8192;
    std::size_t max_payload_length = static_cast<std::size_t>(-1);
};

struct RequestHead {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
};

enum class Event { NeedData, HeadCompleted, BodyChunk, MessageComplete, Failed };

class RequestParser {
public:
    explicit RequestParser(Limits = {});
    void feed(const char* data, std::size_t len);
    void feed_eof();
    Event poll();
    const RequestHead& head() const;
    std::string_view body_chunk() const;
    ParseError error() const;
    void reset();
};

}
```

### Scope deltas from the original plan

- `BodyMode::CloseDelim` removed. RFC 9112 §6.3: request bodies require
  either `Content-Length` or `Transfer-Encoding`; otherwise body length
  is zero. "Read until EOF" is a response-side concern and will live in
  phase 4 (`ResponseParser`). The `BodyWithoutLength_FeedEofTerminates`
  test was repurposed to pin the RFC-correct behaviour: no framing →
  immediate `MessageComplete`, subsequent `feed_eof()` is harmless.
- Added one test beyond the original list:
  `Adversarial_ChunkedOneByteAtATime` — feeds a chunked POST with
  trailers one byte at a time, verifies the assembled body bit-for-bit.

---

## Phase 2 — `ResponseSerializer` (✅ complete)

### What landed

| File | Change |
|---|---|
| [httplib_core.h](httplib_core.h) | Modified — added ~370 lines for `SerializeError`, `ResponseHead`, `detail::status_reason_phrase()`, `detail::to_hex()`, `ResponseSerializer`. |
| [test/test_sansio_serializer.cc](test/test_sansio_serializer.cc) | **New**, 18 tests. |
| [test/CMakeLists.txt](test/CMakeLists.txt) | Additive: new `test-sansio-serializer` target. |

### API

```cpp
namespace httplib::core {

enum class SerializeError {
    None, StatusOutOfRange, InvalidHeaderName, InvalidHeaderValue,
    BodyAfterEnd, WrongBodyMode, ContentLengthMismatch, ContentLengthOverflow,
};

struct ResponseHead {
    int status = 200;
    std::string version = "HTTP/1.1";
    std::vector<std::pair<std::string, std::string>> headers;
};

class ResponseSerializer {
public:
    ResponseSerializer() = default;

    // Push side
    void begin(ResponseHead head);
    void write_body(std::string_view data);
    void end(std::vector<std::pair<std::string, std::string>> trailers = {});

    // Pull side
    std::string_view pending_bytes() const;
    void consume(std::size_t n);
    bool done() const;
    SerializeError error() const;
};

}
```

### Design decisions

1. **Pull side**: `pending_bytes()` + `consume(n)` — simpler than `poll()`, zero-copy
2. **Body framing**: Implicit from headers — symmetric with `RequestParser`
3. **CL mismatch**: Error at `end()` (under) / `write_body()` (over) — no padding, no silent truncation
4. **Default headers** (`Connection`, `Content-Type`, `Server`): None — core is policy-free; injection is phase-3 adapter work
5. **`100 Continue`**: A separate serializer instance — `begin({.status=100})` + `end()` produces `HTTP/1.1 100 Continue\r\n\r\n`. No `send_interim()` method
6. **`set_header_writer()` customization**: Deferred to phase 3 (adapter concern)

### Known gaps to address in phase 3

- **No `reset()` method.** The serializer is one-shot. Phase 3 must either
  construct a fresh `ResponseSerializer` per response or add `reset()`.
- **Malformed `Content-Length` silently parses to 0.** Caller-supplied
  garbage in the CL header (e.g. `"11abc"`) yields `cl_declared_ = 0`,
  which then trips `ContentLengthOverflow` on the first `write_body`.
  Defensible (caller asked for it) but undocumented.
- **`end()` with no prior `begin()`** silently marks `ended_ = true` with
  an empty buffer; `done()` returns true. No error fires.
- **Unknown status codes emit `"HTTP/1.1 999 \r\n"`** (empty reason
  phrase, trailing space before CRLF). Existing `httplib.h` falls back to
  `"Internal Server Error"`. Phase 3 should decide: match the existing
  behaviour, or strip the trailing space.

---

## Test counts

Measured via `./build/test/httplib-test --gtest_list_tests` (the
authoritative count, not `ctest -N` which reports CTest test entries
including parameterized expansions).

| Suite | Before phase 1 | After phase 1 | After phase 2 |
|---|---|---|---|
| `httplib-test` (existing) | 666 | 666 | 666 |
| `test-sansio-parser` | — | 27 | 27 |
| `test-sansio-serializer` | — | — | 18 |
| `httplib-test-fuzz` | 1 | 1 | 1 |
| **CTest total** | 667 | 694 | 712 |

All passing; zero failures, zero skipped.

---

## Phase 3 — rewire (next, **not yet started**)

Make `Server::process_request` drive `RequestParser` + `ResponseSerializer`
through a small `Stream` pump. **This is the high-risk commit** — `httplib.h`
finally changes, and the existing 666-test suite becomes the regression net.

### Scope

- Replace the read-headers / read-body code path in
  [httplib.h:11913](httplib.h:11913) (`Server::process_request`) with a
  `RequestParser`-driven loop.
- Replace `write_response_core` (currently at
  [httplib.h:11088](httplib.h:11088)) with a `ResponseSerializer`-driven
  loop.
- Decide on a `reset()` model vs. per-request instantiation for the
  serializer (see phase 2 known gaps).
- Add `set_header_writer()` shim at the adapter level, not the core.
- Default headers (`Content-Type` fallback, `Content-Length: 0` when
  empty, `Server`, `Connection`) injected here, not in the core.
- 100 Continue / Expect handling stays in the adapter (drive a second
  serializer for the interim status).

### Acceptance criteria

- `cmake --build build && ctest --test-dir build` is fully green.
- `httplib-test` runs **exactly 666 tests**, all passing — proof we
  didn't change observable behaviour.
- `test-sansio-parser` (27) and `test-sansio-serializer` (18) still
  green.
- `httplib_core.h` still compiles standalone with only stdlib headers.
- No new public API on `Server` / `Request` / `Response`. Internal-only
  refactor.

---

## Future phases (sketch)

- **Phase 4**: Client side — `ResponseParser` + `RequestSerializer`,
  then rewire `ClientImpl::process_request`. `ResponseParser` needs
  close-delimited body mode (RFC 9112 §6.3: response bodies *may* lack
  framing, terminated by connection close).
- **Phase 5**: WebSocket frame codec — convert
  `read_websocket_frame` / `write_websocket_frame` to byte-fed.
- **Phase 6**: Hand-rolled `epoll` example proving the core is usable
  without `Server`.

## Verification

```bash
cmake --build build -j$(nproc)
ctest --test-dir build           # full suite, must be green

# Per-suite spot checks
./build/test/httplib-test                  --gtest_list_tests | grep -c '^  '   # → 666
./build/test/test-sansio-parser            --gtest_brief=1                      # → 27 passed
./build/test/test-sansio-serializer        --gtest_brief=1                      # → 18 passed

# Regression invariant — must be empty in phases 1–2
git diff httplib.h test/test.cc example/

# Standalone core compile
g++ -std=c++17 -fsyntax-only httplib_core.h
```
