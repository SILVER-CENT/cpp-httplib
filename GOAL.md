# GOAL — sans-IO core for cpp-httplib

## Vision

Decouple HTTP/1.1 (and WebSocket) **protocol parsing + serialization** from
**server logic** and from **I/O**. The library should expose a pure protocol
state machine: the caller feeds bytes in, the library tells them what
happened (headers complete, body chunk, request complete, error) and asks
for bytes out. *How* those bytes arrive and depart — blocking socket,
non-blocking socket, io_uring, TLS, a unit test buffer, a fuzzer — is the
caller's problem, not the library's.

Inspiration: Python's `h11`, Rust's `httparse` + `http`, Hyper's
`hyper::proto` layer, the "Sans-IO" essay by Brett Cannon.

## Why

Today the protocol code is welded to a blocking `Stream` abstraction
(`httplib::Stream`, `httplib::detail::SocketStream`). That means:

- You cannot drive the parser from an async runtime without a thread per
  connection.
- You cannot run the parser against a fixed in-memory buffer in a fuzzer
  without faking a `Stream` subclass.
- Unit-testing protocol edge cases requires standing up a socket pair.
- The library cannot be embedded into a host that owns its own event loop
  (e.g. a game engine, a coroutine runtime, an embedded system, a
  cooperative scheduler in another library).

A sans-IO core fixes all of the above without removing the existing
batteries-included `Server` / `Client`. Those become thin adapters layered
on top of the core.

## Acceptance criteria

A change is "done" when **all** of the following hold:

1. There exists a `httplib::core` (name TBD) namespace whose types do
   **not** mention `Stream`, socket, file descriptor, TLS handle, thread,
   or `std::function` callbacks that perform I/O.
2. A server-side `RequestParser` (or `Connection`) accepts bytes via a
   pure `feed(const char*, size_t)` style API and yields events
   (`HeadersReady{req}`, `BodyChunk{view}`, `Complete{}`, `Error{e}`) or
   exposes them as pollable state.
3. A `ResponseSerializer` accepts a `Response` (or per-event commands like
   `start_response`, `write_body_chunk`, `end`) and produces bytes into a
   caller-owned buffer — without ever touching a socket.
4. Symmetric pieces exist on the client side (`ResponseParser`,
   `RequestSerializer`).
5. A WebSocket frame codec exposes `feed_bytes` → frame events and
   `encode_frame` → bytes, with no `Stream` reference held.
6. The existing `httplib::Server` and `httplib::Client` are rewritten as
   thin adapters over the sans-IO core and **the full existing test suite
   still passes 100%** with no behavioural regressions visible to user
   code.
7. There is at least one new example under `example/` that drives the
   sans-IO core directly from a hand-rolled epoll/poll loop (or even a
   string-in / string-out unit test) to prove the API is usable without
   the bundled I/O.
8. There are new tests under `test/` that exercise the parser on
   adversarial byte chunkings (one byte at a time, split mid-header,
   split mid-chunk-size, etc.) — the kind of test that is awkward to
   write today because everything goes through a socket.

## Non-goals (for now)

- Not removing or changing the public `Server` / `Client` API. They keep
  working exactly as today; sans-IO is the new layer underneath them.
- Not making the library async / coroutine-aware. That becomes *possible*
  for downstream users, but we are not shipping an async runtime here.
- Not breaking up `httplib.h` into multiple files in this work item —
  that's a separate refactor. Single-header stays, internal layering
  becomes clearer.
- Not changing TLS handling. TLS stays as a `Stream` adapter that wraps
  the same sans-IO core.
- Not touching the routing / handler / matcher subsystem (`MatcherBase`,
  `PathParamsMatcher`, etc.). Those run on top of a complete `Request`
  and are already I/O-free.

## What "sans-IO" looks like for this codebase

The clean cut sits **between `Stream` and the parsers/serializers**.

Today:

```
SocketStream  ──┐
BufferStream  ──┼──► Stream& ──► stream_line_reader ──► parse_request_line
                │              ──► read_headers
                │              ──► read_content_*  (with ChunkedDecoder holding Stream&)
                └──► write_request_line / write_response_line / write_headers
                     write_content_*
```

After:

```
                                     ┌── bytes ──► RequestParser ──► events
caller-owned buffer / socket / etc. ─┤
                                     └◄─ bytes ── ResponseSerializer ◄── set_response / write_body
```

And `Stream`-shaped helpers become a ~50-line adapter that pumps bytes
between a `Stream&` and the new parser/serializer. The existing
`Server::process_request` becomes that adapter plus the routing call.

## Phased plan

See [PLAN.md](PLAN.md) for the executable plan, phase status, conventions
locked in, and acceptance criteria. The phases break down as:

1. **Request parser** — pure byte-fed state machine, no `Stream`.
2. **Response serializer** — pure byte-emitting state machine, no `Stream`.
3. **Adapt `Server::process_request`** — thin pump between `Stream` and the core.
4. **Repeat (1)–(3) for the client side.**
5. **WebSocket frame codec** — same pattern.
6. **Hand-rolled-loop example** proving the core is usable without `Server`.

## Verification

- `cmake --build build && ctest --test-dir build` is the source of truth.
  Every phase keeps the suite green.
- New work adds tests, never deletes them.

## Initial exploration — where the I/O coupling lives

Snapshot from the first pass through `httplib.h` (lines approximate, may
drift):

- **The I/O boundary** is `class Stream` at [httplib.h:1433](httplib.h:1433)
  — `read()` / `write()` virtuals. Two implementations: `SocketStream`
  ([httplib.h:5534](httplib.h:5534)) and `BufferStream`
  ([httplib.h:3039](httplib.h:3039)). `BufferStream` is already an
  in-memory `Stream` — useful prior art and a sanity check that the
  abstraction can be driven without a socket.
- **Server-side parse pipeline** lives in
  `Server::process_request` ([httplib.h:11913](httplib.h:11913)):
  - reads request line via `stream_line_reader`
    ([httplib.h:3178](httplib.h:3178), impl 5229)
  - parses it with `parse_request_line` ([httplib.h:11017](httplib.h:11017))
  - reads headers with `detail::read_headers(Stream&, Headers&)`
    ([httplib.h:7018](httplib.h:7018))
  - reads body with `read_content_with_length` /
    `read_content_without_length` / `read_content_chunked`
    ([httplib.h:7120](httplib.h:7120), 7162, 7187)
  - chunked decoding goes through `detail::ChunkedDecoder` which
    **holds a `Stream&` member** ([httplib.h:3200](httplib.h:3200)) —
    this is one of the bigger refactors: it needs to become a byte-fed
    state machine.
- **Server-side serialize pipeline** lives in
  `Server::write_response_core` ([httplib.h:11088](httplib.h:11088)):
  - `detail::write_response_line(Stream&, int)`
    ([httplib.h:7347](httplib.h:7347))
  - `header_writer_(Stream&, Headers&)` (defaults to
    `detail::write_headers`, [httplib.h:7356](httplib.h:7356))
  - Body via `write_content_with_provider`
    ([httplib.h:11161](httplib.h:11161))
  - Already buffers small responses through a `BufferStream` before
    flushing ([httplib.h:11131](httplib.h:11131)). That pattern is
    basically a tiny sans-IO mode already and is encouraging.
- **Client-side mirror** lives in `ClientImpl::process_request`
  ([httplib.h:13547](httplib.h:13547)) — same shape, opposite direction.
- **Keep-alive loop** is `detail::process_server_socket_core`
  ([httplib.h:5606](httplib.h:5606)) — calls a `callback(Stream&, ...)`
  per request. After the refactor, this callback becomes "feed bytes
  into the parser; when a request event lands, run routing; ask the
  serializer for response bytes; write them". The loop itself stays
  blocking and `Stream`-based — only the *protocol work* inside it
  becomes sans-IO.
- **WebSocket** is symmetric:
  `detail::read_websocket_frame(Stream&, ...)` at
  [httplib.h:4767](httplib.h:4767) and
  `detail::write_websocket_frame(Stream&, ...)` at
  [httplib.h:4701](httplib.h:4701). Both single functions, both pull/push
  from `Stream`. Good candidates to convert to byte-fed codecs.
- **Already pure** (no `Stream` dependency, safe to keep as-is):
  - `parse_header` ([httplib.h:4984](httplib.h:4984)) — pointer-range based.
  - `parse_request_line` ([httplib.h:11017](httplib.h:11017)) — operates on
    `const char*`. (Bonus: it can move to the core as-is.)
  - Routing / matching (`MatcherBase`,
    [httplib.h:1528](httplib.h:1528) onward).
  - Compression codecs ([httplib.h:3061](httplib.h:3061) onward).
  - URL parsing, multipart parsing helpers — mostly string-in / string-out.

That asymmetry — line parsing is already pure, but it's fed via a
`Stream`-aware `stream_line_reader` — is the shape of the whole problem
in miniature. Most of the *formatting* logic is fine. It's the *pulling
of bytes* that needs to become *being-fed bytes*.

## Open questions

- API style for the parser: event-yielding (h11-like
  `next_event() -> Event`) vs. callback-on-feed
  (`feed(bytes, on_event)`) vs. polled state machine
  (`feed(bytes); while (auto e = poll_event()) {...}`)? Lean toward the
  polled style — simplest to implement in C++ without coroutines.
- How to expose body data: copy into `std::string` (matches today's
  `Request::body`) vs. zero-copy `std::string_view` into the feed buffer
  (faster, but caller must respect lifetimes)? Probably offer both.
- Where to put the new code: a new `namespace httplib::core` inside
  `httplib.h`, or a new internal header pulled in by `httplib.h`? Header
  layout question — decide once shape is clear.
