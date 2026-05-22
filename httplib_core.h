//
//  httplib_core.h
//
//  Sans-IO core for cpp-httplib. Pure HTTP/1.1 protocol state machines —
//  bytes in, events out, bytes out. No sockets, no Stream, no threads, no
//  TLS, no callbacks that perform I/O. See GOAL.md and PLAN.md.
//
//  Phase 1: server-side RequestParser only.
//    - Step 6 (this commit): real request-line + headers state machine,
//      with body-mode classification at end-of-head and immediate
//      completion for zero-length bodies. Length-known and chunked body
//      parsing are still stubbed and will land in step 7.
//
//  Requires C++17 (std::string_view).
//

#ifndef CPPHTTPLIB_HTTPLIB_CORE_H
#define CPPHTTPLIB_HTTPLIB_CORE_H

#if __cplusplus < 201703L
#error "httplib_core.h requires C++17 or later"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httplib {
namespace core {

// ---------------------------------------------------------------------------
// ParseError — every way the request parser can refuse a byte stream.
// `None` is the resting state (no error). The remaining values mirror the
// existing httplib::Error categories that apply to inbound HTTP/1.1 parsing.
// ---------------------------------------------------------------------------
enum class ParseError {
  None = 0,
  InvalidRequestLine,
  InvalidHTTPMethod,
  InvalidHTTPVersion,
  InvalidHeaders,
  ExceedHeaderMaxLength,
  ExceedHeaderMaxCount,
  ExceedUriMaxLength,
  ExceedMaxPayloadSize,
  InvalidChunkedEncoding,
  ContentLengthAndTransferEncoding,
  UnexpectedEof,
};

// ---------------------------------------------------------------------------
// Limits — caller-controlled bounds on what the parser will accept. Defaults
// mirror the CPPHTTPLIB_*_MAX_* constants in httplib.h.
// ---------------------------------------------------------------------------
struct Limits {
  std::size_t max_header_length = 8192;     // per-line cap (CPPHTTPLIB_HEADER_MAX_LENGTH)
  std::size_t max_header_count = 100;       // CPPHTTPLIB_HEADER_MAX_COUNT
  std::size_t max_uri_length = 8192;        // CPPHTTPLIB_REQUEST_URI_MAX_LENGTH
  std::size_t max_payload_length =          // CPPHTTPLIB_PAYLOAD_MAX_LENGTH
      static_cast<std::size_t>(-1);
};

// ---------------------------------------------------------------------------
// RequestHead — everything we've parsed before the body. `headers` is an
// ordered vector of (name, value) pairs so the parser can stay independent
// of httplib::Headers (case-insensitive multimap). A bridge function can
// convert to httplib::Headers when the adapter layer wires this in.
// ---------------------------------------------------------------------------
struct RequestHead {
  std::string method;
  std::string target;   // raw request-target, before path/query split
  std::string version;  // "HTTP/1.0" or "HTTP/1.1"
  std::vector<std::pair<std::string, std::string>> headers;
  // RFC 9112 §6.5 trailer fields, populated only for chunked requests that
  // include a trailer section. Empty for length-known and zero-length bodies.
  std::vector<std::pair<std::string, std::string>> trailers;
};

// ---------------------------------------------------------------------------
// Event — what poll() returns. The parser is a pull-based state machine: the
// caller feeds bytes and polls, the parser advances and yields events.
//
//   NeedData         — buffer doesn't have enough bytes to make progress
//   HeadCompleted    — head() is now populated and valid
//   BodyChunk        — body_chunk() is valid for THIS poll() call only
//   MessageComplete  — request fully consumed; call reset() before the next
//   Failed           — error() tells you what went wrong; parser is stuck
// ---------------------------------------------------------------------------
enum class Event {
  NeedData,
  HeadCompleted,
  BodyChunk,
  MessageComplete,
  Failed,
};

namespace detail {

// RFC 9110 §5.6.2 token-char set. Borrowed in spirit from
// httplib::detail::fields::is_token_char so behaviour matches.
inline bool is_token_char(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  if (std::isalnum(uc)) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

inline bool is_token(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!is_token_char(c)) return false;
  }
  return true;
}

// RFC 9110 §5.5 field-value: VCHAR / SP / HTAB / obs-text. No CR/LF/NUL.
inline bool is_field_value_char(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  if (uc >= 0x21 && uc <= 0x7E) return true;        // VCHAR
  if (uc >= 0x80) return true;                       // obs-text
  if (c == ' ' || c == '\t') return true;
  return false;
}

inline bool is_field_value(std::string_view s) {
  for (char c : s) {
    if (!is_field_value_char(c)) return false;
  }
  return true;
}

inline bool is_space_or_tab(char c) { return c == ' ' || c == '\t'; }

// Methods accepted by the existing Server::parse_request_line; preserved
// here so the sans-IO parser doesn't accidentally accept stuff the
// existing server would have rejected.
inline bool is_known_method(std::string_view m) {
  return m == "GET"     || m == "HEAD"    || m == "POST"
      || m == "PUT"     || m == "DELETE"  || m == "CONNECT"
      || m == "OPTIONS" || m == "TRACE"   || m == "PATCH"
      || m == "PRI";
}

// Locate the first CRLF at or after `from` inside `buf`. Returns the index
// of the '\r', or std::string::npos if no complete CRLF lies in the
// available bytes.
inline std::size_t find_crlf(const std::string &buf, std::size_t from) {
  for (std::size_t i = from; i + 1 < buf.size(); ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n') return i;
  }
  return std::string::npos;
}

inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

} // namespace detail

// ---------------------------------------------------------------------------
// RequestParser — server-side inbound HTTP/1.1 parser. Sans-IO: no Stream,
// no socket, no callbacks. Caller drives via feed()/poll().
// ---------------------------------------------------------------------------
class RequestParser {
public:
  RequestParser() = default;
  explicit RequestParser(Limits limits) : limits_(limits) {}

  // Append bytes. Cheap — buffers them internally and advances a small
  // amount of state. Real work happens in poll().
  void feed(const char *data, std::size_t len);

  // Signal that the peer half-closed. Relevant for HTTP/1.0
  // close-delimited bodies; for length-known or chunked bodies, an EOF
  // before completion produces ParseError::UnexpectedEof.
  void feed_eof();

  // Advance the state machine and return the next event. Does not consume
  // unrelated state — calling poll() repeatedly returns NeedData until the
  // caller feeds more bytes (or feed_eof()).
  Event poll();

  // Valid after the first HeadCompleted of the current request. Reference
  // remains stable until reset() (or another HeadCompleted after reset()).
  const RequestHead &head() const { return head_; }

  // Valid only for the poll() call that returned BodyChunk.
  std::string_view body_chunk() const { return body_chunk_; }

  ParseError error() const { return error_; }

  // Prepare to parse the next request on the same connection (keep-alive).
  // Preserves any buffered bytes already past the previous request's body
  // — those belong to the next request and will be consumed on the next
  // poll().
  void reset();

  const Limits &limits() const { return limits_; }

private:
  enum class State {
    ReadingRequestLine,
    ReadingHeaders,
    ReadingBody,
    Complete,
    Errored,
  };

  enum class BodyMode {
    None,        // no Content-Length, no Transfer-Encoding → zero-length body
    Length,      // Content-Length: N
    Chunked,     // Transfer-Encoding: chunked
  };

  // Sub-states of the chunked-body sub-machine. Only meaningful when
  // body_mode_ == Chunked and state_ == ReadingBody.
  enum class ChunkedSub {
    ReadingSize,        // parsing the next "chunk-size [;ext] CRLF" line
    ReadingData,        // streaming out the current chunk's payload bytes
    ReadingDataCrlf,    // consuming the CRLF that follows chunk data
    ReadingTrailers,    // optional trailer-section header lines
  };

  // Try to consume the request line from buffer_[cursor_..]. Returns
  // true if a line was consumed (state advanced). Returns false on
  // NeedData (no full line yet) or error (state set to Errored).
  bool step_request_line(bool &made_progress);

  // Try to consume one header line. Returns true on progress, false on
  // NeedData / error. Sets `end_of_headers = true` if the empty line
  // terminating the head was consumed.
  bool step_header_line(bool &made_progress, bool &end_of_headers);

  // Body-stepping helpers. Each returns one of three things via outputs:
  //   - sets `emitted_chunk = true` and updates body_chunk_ → BodyChunk
  //   - sets `done = true` → caller should emit MessageComplete
  //   - returns false → NeedData / Failed (state may be set to Errored)
  bool step_body_length(bool &emitted_chunk, bool &done);
  bool step_body_chunked(bool &emitted_chunk, bool &done);

  // Called once the empty line ending the head is consumed. Classifies
  // body mode based on Content-Length / Transfer-Encoding. May set an
  // error (e.g. both headers present).
  void classify_body_mode();

  // Convenience: set error and transition to Errored.
  Event fail(ParseError e) {
    error_ = e;
    state_ = State::Errored;
    return Event::Failed;
  }

  Limits limits_{};
  State state_ = State::ReadingRequestLine;
  BodyMode body_mode_ = BodyMode::None;
  ParseError error_ = ParseError::None;
  bool eof_seen_ = false;
  bool head_emitted_ = false;  // true between HeadCompleted emission and reset()

  std::string buffer_;
  std::size_t cursor_ = 0;     // first unconsumed byte in buffer_

  // Body bookkeeping.
  std::size_t body_remaining_ = 0;   // Length mode: bytes left in the body.
                                     // Chunked mode: bytes left in current chunk.
  std::size_t body_received_ = 0;    // Cumulative payload bytes already streamed
                                     // out as BodyChunk events; for payload-cap
                                     // enforcement in chunked mode.
  ChunkedSub chunked_sub_ = ChunkedSub::ReadingSize;

  RequestHead head_{};
  std::string_view body_chunk_{};
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline void RequestParser::feed(const char *data, std::size_t len) {
  if (data == nullptr || len == 0) { return; }
  buffer_.append(data, len);
}

inline void RequestParser::feed_eof() { eof_seen_ = true; }

inline Event RequestParser::poll() {
  body_chunk_ = std::string_view{};

  for (;;) {
    switch (state_) {
      case State::Errored:
        return Event::Failed;

      case State::Complete:
        return Event::MessageComplete;

      case State::ReadingRequestLine: {
        bool progress = false;
        if (!step_request_line(progress)) {
          if (state_ == State::Errored) return Event::Failed;
          // If the peer half-closed before we could finish the head, the
          // request is truncated. (Covers both "EOF with empty buffer" and
          // "EOF with a partial request line and no CRLF in sight".)
          if (eof_seen_) {
            return fail(ParseError::UnexpectedEof);
          }
          return Event::NeedData;
        }
        // Progress: fall through to the loop iteration that handles headers.
        continue;
      }

      case State::ReadingHeaders: {
        bool progress = false;
        bool end_of_headers = false;
        if (!step_header_line(progress, end_of_headers)) {
          if (state_ == State::Errored) return Event::Failed;
          if (eof_seen_) {
            return fail(ParseError::UnexpectedEof);
          }
          return Event::NeedData;
        }
        if (end_of_headers) {
          classify_body_mode();
          if (state_ == State::Errored) return Event::Failed;
          head_emitted_ = true;
          // Where to next? If there's no body, we'll emit MessageComplete
          // on the next poll(); for now, just emit HeadCompleted.
          if (body_mode_ == BodyMode::None) {
            state_ = State::Complete;
          } else {
            state_ = State::ReadingBody;
          }
          return Event::HeadCompleted;
        }
        // Consumed one header line; loop for more.
        continue;
      }

      case State::ReadingBody: {
        bool emitted_chunk = false;
        bool done = false;
        bool ok = (body_mode_ == BodyMode::Chunked)
                      ? step_body_chunked(emitted_chunk, done)
                      : step_body_length(emitted_chunk, done);
        if (!ok) {
          if (state_ == State::Errored) return Event::Failed;
          if (eof_seen_) {
            return fail(ParseError::UnexpectedEof);
          }
          return Event::NeedData;
        }
        if (emitted_chunk) {
          return Event::BodyChunk;
        }
        if (done) {
          state_ = State::Complete;
          return Event::MessageComplete;
        }
        // Made progress but no chunk to emit yet (e.g. consumed a chunk-size
        // line in chunked mode). Loop again.
        continue;
      }
    }
  }
}

inline void RequestParser::reset() {
  // Preserve any unconsumed bytes after the previous request — they belong
  // to the next request on a keep-alive connection (pipelining).
  if (cursor_ < buffer_.size()) {
    buffer_.erase(0, cursor_);
  } else {
    buffer_.clear();
  }
  cursor_ = 0;
  state_ = State::ReadingRequestLine;
  body_mode_ = BodyMode::None;
  body_remaining_ = 0;
  body_received_ = 0;
  chunked_sub_ = ChunkedSub::ReadingSize;
  error_ = ParseError::None;
  head_emitted_ = false;
  head_ = RequestHead{};
  body_chunk_ = std::string_view{};
  // Note: eof_seen_ is sticky for the connection. Once the peer closed,
  // it doesn't un-close. Caller controls this via feed_eof().
}

inline bool RequestParser::step_request_line(bool &made_progress) {
  auto crlf = detail::find_crlf(buffer_, cursor_);
  if (crlf == std::string::npos) {
    // Defensive limit: if the unterminated prefix is already larger than a
    // reasonable request line could be (method + SP + URI + SP + version
    // + CRLF), refuse. We use max_uri_length plus generous slack.
    if (buffer_.size() - cursor_ >
        limits_.max_uri_length + 32) {
      error_ = ParseError::ExceedUriMaxLength;
      state_ = State::Errored;
    }
    return false;
  }

  std::string_view line(buffer_.data() + cursor_, crlf - cursor_);

  // Three space-separated components: METHOD SP TARGET SP VERSION.
  auto p1 = line.find(' ');
  if (p1 == std::string_view::npos) {
    error_ = ParseError::InvalidRequestLine;
    state_ = State::Errored;
    return false;
  }
  auto p2 = line.find(' ', p1 + 1);
  if (p2 == std::string_view::npos) {
    error_ = ParseError::InvalidRequestLine;
    state_ = State::Errored;
    return false;
  }
  if (line.find(' ', p2 + 1) != std::string_view::npos) {
    error_ = ParseError::InvalidRequestLine;
    state_ = State::Errored;
    return false;
  }

  auto method_sv = line.substr(0, p1);
  auto target_sv = line.substr(p1 + 1, p2 - p1 - 1);
  auto version_sv = line.substr(p2 + 1);

  if (target_sv.empty()) {
    error_ = ParseError::InvalidRequestLine;
    state_ = State::Errored;
    return false;
  }

  if (target_sv.size() > limits_.max_uri_length) {
    error_ = ParseError::ExceedUriMaxLength;
    state_ = State::Errored;
    return false;
  }

  if (!detail::is_known_method(method_sv)) {
    // Method must also be a syntactically valid token; this is the stricter
    // failure mode that matches the existing Server::parse_request_line.
    if (!detail::is_token(method_sv)) {
      error_ = ParseError::InvalidRequestLine;
      state_ = State::Errored;
      return false;
    }
    error_ = ParseError::InvalidHTTPMethod;
    state_ = State::Errored;
    return false;
  }

  if (version_sv != "HTTP/1.0" && version_sv != "HTTP/1.1") {
    error_ = ParseError::InvalidHTTPVersion;
    state_ = State::Errored;
    return false;
  }

  head_.method.assign(method_sv);
  head_.target.assign(target_sv);
  head_.version.assign(version_sv);

  cursor_ = crlf + 2;
  state_ = State::ReadingHeaders;
  made_progress = true;
  return true;
}

inline bool RequestParser::step_header_line(bool &made_progress,
                                            bool &end_of_headers) {
  auto crlf = detail::find_crlf(buffer_, cursor_);
  if (crlf == std::string::npos) {
    if (buffer_.size() - cursor_ > limits_.max_header_length) {
      error_ = ParseError::ExceedHeaderMaxLength;
      state_ = State::Errored;
    }
    return false;
  }

  std::size_t line_len = crlf - cursor_;
  if (line_len == 0) {
    // Empty line terminates the head.
    cursor_ = crlf + 2;
    made_progress = true;
    end_of_headers = true;
    return true;
  }

  if (line_len > limits_.max_header_length) {
    error_ = ParseError::ExceedHeaderMaxLength;
    state_ = State::Errored;
    return false;
  }

  if (head_.headers.size() >= limits_.max_header_count) {
    error_ = ParseError::ExceedHeaderMaxCount;
    state_ = State::Errored;
    return false;
  }

  std::string_view line(buffer_.data() + cursor_, line_len);

  // Parse "name : value" with trimming of trailing OWS on value, and
  // leading OWS after the colon.
  auto colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    error_ = ParseError::InvalidHeaders;
    state_ = State::Errored;
    return false;
  }

  auto name = line.substr(0, colon);
  // RFC 7230 §3.2.4: no whitespace allowed between field name and colon.
  if (!detail::is_token(name)) {
    error_ = ParseError::InvalidHeaders;
    state_ = State::Errored;
    return false;
  }

  auto value = line.substr(colon + 1);
  while (!value.empty() && detail::is_space_or_tab(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && detail::is_space_or_tab(value.back())) {
    value.remove_suffix(1);
  }

  if (!detail::is_field_value(value)) {
    error_ = ParseError::InvalidHeaders;
    state_ = State::Errored;
    return false;
  }

  head_.headers.emplace_back(std::string(name), std::string(value));
  cursor_ = crlf + 2;
  made_progress = true;
  return true;
}

inline void RequestParser::classify_body_mode() {
  bool has_te = false;
  bool te_is_chunked = false;
  bool has_cl = false;
  std::size_t cl_value = 0;
  bool cl_value_set = false;

  for (const auto &kv : head_.headers) {
    if (detail::iequals(kv.first, "Transfer-Encoding")) {
      has_te = true;
      // The existing httplib code triggers on the presence of any TE; we
      // also remember whether it explicitly named `chunked` (the only
      // mode RFC 9112 requires us to implement).
      if (detail::iequals(kv.second, "chunked")) {
        te_is_chunked = true;
      }
    } else if (detail::iequals(kv.first, "Content-Length")) {
      // RFC 9110 §8.6: multiple Content-Length headers must all agree.
      char *endp = nullptr;
      auto v = kv.second;
      // Reject leading/trailing whitespace (already trimmed) and non-digit.
      if (v.empty()) {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return;
      }
      for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          error_ = ParseError::InvalidHeaders;
          state_ = State::Errored;
          return;
        }
      }
      auto parsed = std::strtoull(v.c_str(), &endp, 10);
      if (endp == nullptr || *endp != '\0') {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return;
      }
      if (cl_value_set && parsed != cl_value) {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return;
      }
      cl_value = static_cast<std::size_t>(parsed);
      cl_value_set = true;
      has_cl = true;
    }
  }

  // RFC 9112 §6.3 smuggling defense, matching the existing server
  // behaviour: reject if Content-Length is nonzero AND any Transfer-Encoding
  // header is present. Content-Length: 0 is tolerated for compatibility.
  if (has_cl && cl_value > 0 && has_te) {
    error_ = ParseError::ContentLengthAndTransferEncoding;
    state_ = State::Errored;
    return;
  }

  if (has_te && te_is_chunked) {
    body_mode_ = BodyMode::Chunked;
  } else if (has_te) {
    // Some other transfer-coding we don't recognise.
    error_ = ParseError::InvalidChunkedEncoding;
    state_ = State::Errored;
    return;
  } else if (has_cl) {
    if (cl_value > limits_.max_payload_length) {
      error_ = ParseError::ExceedMaxPayloadSize;
      state_ = State::Errored;
      return;
    }
    body_remaining_ = cl_value;
    body_mode_ = (cl_value == 0) ? BodyMode::None : BodyMode::Length;
  } else {
    // No length indicators: zero-length body for request-side HTTP/1.1.
    body_mode_ = BodyMode::None;
  }
}

// ---------------------------------------------------------------------------
// Body parsing — Content-Length mode. Stream out whatever bytes are buffered,
// up to body_remaining_. When body_remaining_ hits zero, the message is done.
// ---------------------------------------------------------------------------
inline bool RequestParser::step_body_length(bool &emitted_chunk, bool &done) {
  if (body_remaining_ == 0) {
    done = true;
    return true;
  }
  auto available = buffer_.size() - cursor_;
  if (available == 0) {
    return false;  // NeedData
  }
  auto take = (available < body_remaining_) ? available : body_remaining_;
  body_chunk_ = std::string_view(buffer_.data() + cursor_, take);
  cursor_ += take;
  body_remaining_ -= take;
  body_received_ += take;
  emitted_chunk = true;
  return true;
}

// ---------------------------------------------------------------------------
// Body parsing — chunked transfer-encoding. RFC 9112 §7.1.
//
//   chunked-body   = *chunk last-chunk trailer-section CRLF
//   chunk          = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
//   last-chunk     = 1*("0") [ chunk-ext ] CRLF
//   trailer-section = *( field-line CRLF )
//
// We don't parse chunk extensions; we ignore everything after the first ';'
// (or after whitespace) on a chunk-size line. Trailers are appended to
// head_.trailers using the same syntactic rules as headers.
// ---------------------------------------------------------------------------
inline bool RequestParser::step_body_chunked(bool &emitted_chunk, bool &done) {
  switch (chunked_sub_) {
    case ChunkedSub::ReadingSize: {
      auto crlf = detail::find_crlf(buffer_, cursor_);
      if (crlf == std::string::npos) {
        // Defensive: a chunk-size line shouldn't be absurdly long.
        if (buffer_.size() - cursor_ > limits_.max_header_length) {
          error_ = ParseError::InvalidChunkedEncoding;
          state_ = State::Errored;
        }
        return false;
      }
      std::string_view line(buffer_.data() + cursor_, crlf - cursor_);

      // Trim chunk extensions (everything from the first ';' onward) and
      // any trailing OWS. RFC 9112 §7.1.1 allows BWS before ';' too.
      auto sep = line.find(';');
      if (sep != std::string_view::npos) line = line.substr(0, sep);
      while (!line.empty() && detail::is_space_or_tab(line.back())) {
        line.remove_suffix(1);
      }
      while (!line.empty() && detail::is_space_or_tab(line.front())) {
        line.remove_prefix(1);
      }

      if (line.empty()) {
        error_ = ParseError::InvalidChunkedEncoding;
        state_ = State::Errored;
        return false;
      }

      // Parse hex chunk-size.
      std::size_t size = 0;
      for (char c : line) {
        unsigned char uc = static_cast<unsigned char>(c);
        int digit;
        if (uc >= '0' && uc <= '9')      digit = uc - '0';
        else if (uc >= 'a' && uc <= 'f') digit = uc - 'a' + 10;
        else if (uc >= 'A' && uc <= 'F') digit = uc - 'A' + 10;
        else {
          error_ = ParseError::InvalidChunkedEncoding;
          state_ = State::Errored;
          return false;
        }
        // Overflow guard: limit chunk-size to max_payload_length so a
        // hostile peer can't make us allocate or read forever.
        if (size > (limits_.max_payload_length >> 4)) {
          error_ = ParseError::ExceedMaxPayloadSize;
          state_ = State::Errored;
          return false;
        }
        size = (size << 4) | static_cast<std::size_t>(digit);
      }

      cursor_ = crlf + 2;

      if (size == 0) {
        // last-chunk → move into trailer section.
        chunked_sub_ = ChunkedSub::ReadingTrailers;
      } else {
        // Enforce cumulative payload cap.
        if (size > limits_.max_payload_length ||
            body_received_ > limits_.max_payload_length - size) {
          error_ = ParseError::ExceedMaxPayloadSize;
          state_ = State::Errored;
          return false;
        }
        body_remaining_ = size;
        chunked_sub_ = ChunkedSub::ReadingData;
      }
      return true;  // progress, but no chunk emitted yet
    }

    case ChunkedSub::ReadingData: {
      auto available = buffer_.size() - cursor_;
      if (available == 0) return false;  // NeedData
      auto take = (available < body_remaining_) ? available : body_remaining_;
      body_chunk_ = std::string_view(buffer_.data() + cursor_, take);
      cursor_ += take;
      body_remaining_ -= take;
      body_received_ += take;
      if (body_remaining_ == 0) {
        chunked_sub_ = ChunkedSub::ReadingDataCrlf;
      }
      emitted_chunk = true;
      return true;
    }

    case ChunkedSub::ReadingDataCrlf: {
      if (buffer_.size() - cursor_ < 2) return false;  // NeedData
      if (buffer_[cursor_] != '\r' || buffer_[cursor_ + 1] != '\n') {
        error_ = ParseError::InvalidChunkedEncoding;
        state_ = State::Errored;
        return false;
      }
      cursor_ += 2;
      chunked_sub_ = ChunkedSub::ReadingSize;
      return true;
    }

    case ChunkedSub::ReadingTrailers: {
      auto crlf = detail::find_crlf(buffer_, cursor_);
      if (crlf == std::string::npos) {
        if (buffer_.size() - cursor_ > limits_.max_header_length) {
          error_ = ParseError::ExceedHeaderMaxLength;
          state_ = State::Errored;
        }
        return false;
      }
      std::size_t line_len = crlf - cursor_;
      if (line_len == 0) {
        // Empty line ends the trailer section → message complete.
        cursor_ = crlf + 2;
        done = true;
        return true;
      }
      if (line_len > limits_.max_header_length) {
        error_ = ParseError::ExceedHeaderMaxLength;
        state_ = State::Errored;
        return false;
      }
      std::string_view line(buffer_.data() + cursor_, line_len);
      auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return false;
      }
      auto name = line.substr(0, colon);
      if (!detail::is_token(name)) {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return false;
      }
      auto value = line.substr(colon + 1);
      while (!value.empty() && detail::is_space_or_tab(value.front())) {
        value.remove_prefix(1);
      }
      while (!value.empty() && detail::is_space_or_tab(value.back())) {
        value.remove_suffix(1);
      }
      if (!detail::is_field_value(value)) {
        error_ = ParseError::InvalidHeaders;
        state_ = State::Errored;
        return false;
      }
      head_.trailers.emplace_back(std::string(name), std::string(value));
      cursor_ = crlf + 2;
      return true;
    }
  }
  return false;  // unreachable
}

} // namespace core
} // namespace httplib

// ===========================================================================
// Phase 2: server-side ResponseSerializer
// ===========================================================================

namespace httplib {
namespace core {

// ---------------------------------------------------------------------------
// SerializeError — every way the response serializer can refuse a caller
// request. `None` is the resting state (no error).
// ---------------------------------------------------------------------------
enum class SerializeError {
  None = 0,
  StatusOutOfRange,        // status < 100 or status > 999
  InvalidHeaderName,       // header name fails is_token()
  InvalidHeaderValue,      // header value fails is_field_value()
  BodyAfterEnd,            // write_body() called after end()
  WrongBodyMode,           // write_body() with data but no CL / TE header
  ContentLengthMismatch,   // at end(): wrote != declared Content-Length
  ContentLengthOverflow,   // at write_body(): cumulative > declared CL
};

// ---------------------------------------------------------------------------
// ResponseHead — everything the caller declares before the body. Same layout
// conventions as RequestHead (ordered vector of pairs, no case-insensitive
// comparator).
// ---------------------------------------------------------------------------
struct ResponseHead {
  int status = 200;
  std::string version = "HTTP/1.1";
  std::vector<std::pair<std::string, std::string>> headers;
};

namespace detail {

// Standalone reason-phrase lookup so httplib_core.h stays independent of
// httplib.h. Covers all IANA-registered HTTP status codes. Returns "" for
// unknown codes (the serializer still emits them — reason phrases are
// optional in HTTP/1.1).
inline std::string_view status_reason_phrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default:  return "";
  }
}

// Hex formatter for chunk-size lines. Mirrors the existing
// detail::from_i_to_hex in httplib.h.
inline std::string to_hex(std::size_t n) {
  static const char charset[] = "0123456789abcdef";
  std::string ret;
  do {
    ret = charset[n & 15] + ret;
    n >>= 4;
  } while (n > 0);
  return ret;
}

} // namespace detail

// ---------------------------------------------------------------------------
// ResponseSerializer — server-side outbound HTTP/1.1 serializer. Sans-IO:
// no Stream, no socket, no callbacks. Caller drives via begin()/write_body()/
// end() (push side) and pending_bytes()/consume() (pull side).
// ---------------------------------------------------------------------------
class ResponseSerializer {
public:
  ResponseSerializer() = default;

  // --- Push side ---

  // Serialize the status-line + headers + CRLF into the internal buffer.
  // Inspects headers to determine body mode:
  //   - "Content-Length: N" → expects exactly N body bytes via write_body()
  //   - "Transfer-Encoding: chunked" → wraps each write_body() as one chunk
  //   - neither → no body allowed; caller must call end() immediately
  void begin(ResponseHead head);

  // Append body bytes. In chunked mode, each non-empty call emits one chunk.
  // Zero-length calls are silently dropped to avoid emitting a premature
  // terminating 0-chunk.
  void write_body(std::string_view data);

  // Finalize the message:
  //   - Content-Length mode: validates byte count matches declaration
  //   - Chunked mode: emits 0-chunk + optional trailers + final CRLF
  //   - No-body mode: no-op (message was already complete after begin())
  void end(std::vector<std::pair<std::string, std::string>> trailers = {});

  // --- Pull side ---

  // View into the serializer's internal buffer. Valid until the next
  // push-side call or consume(). Empty when nothing is pending.
  std::string_view pending_bytes() const;

  // Drop n bytes from the front of the pending buffer.
  void consume(std::size_t n);

  // True after end() has been called AND all pending bytes consumed.
  bool done() const;

  // Error state. Once set, all further push calls are no-ops.
  SerializeError error() const { return error_; }

private:
  enum class BodyMode {
    None,      // no body framing → write_body() is an error
    Length,    // Content-Length: N
    Chunked,   // Transfer-Encoding: chunked
  };

  // Convenience: set error and stop accepting further input.
  void fail(SerializeError e) { error_ = e; }

  SerializeError error_ = SerializeError::None;
  BodyMode body_mode_ = BodyMode::None;
  bool begun_ = false;
  bool ended_ = false;

  std::string buffer_;
  std::size_t read_pos_ = 0;  // first unconsumed byte in buffer_

  // Content-Length bookkeeping
  std::size_t cl_declared_ = 0;  // declared Content-Length value
  std::size_t cl_written_ = 0;   // cumulative bytes written via write_body()
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline void ResponseSerializer::begin(ResponseHead head) {
  if (error_ != SerializeError::None) return;

  // Validate status code range (100-999).
  if (head.status < 100 || head.status > 999) {
    fail(SerializeError::StatusOutOfRange);
    return;
  }

  // Validate all header names and values.
  for (const auto& kv : head.headers) {
    if (!detail::is_token(kv.first)) {
      fail(SerializeError::InvalidHeaderName);
      return;
    }
    if (!detail::is_field_value(kv.second)) {
      fail(SerializeError::InvalidHeaderValue);
      return;
    }
  }

  // Serialize status line: "HTTP/1.1 200 OK\r\n"
  buffer_ += head.version;
  buffer_ += ' ';
  buffer_ += std::to_string(head.status);
  buffer_ += ' ';
  buffer_ += detail::status_reason_phrase(head.status);
  buffer_ += "\r\n";

  // Serialize headers: "name: value\r\n" each, in insertion order.
  for (const auto& kv : head.headers) {
    buffer_ += kv.first;
    buffer_ += ": ";
    buffer_ += kv.second;
    buffer_ += "\r\n";
  }

  // Empty line terminates the head.
  buffer_ += "\r\n";

  // Classify body mode from headers (implicit, matching RequestParser).
  bool has_cl = false;
  bool has_te_chunked = false;

  for (const auto& kv : head.headers) {
    if (detail::iequals(kv.first, "Content-Length")) {
      has_cl = true;
      // Parse the value.
      std::size_t val = 0;
      for (char c : kv.second) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          // Non-digit in CL value — not our problem to error on here,
          // the caller provided it. Treat as zero.
          val = 0;
          break;
        }
        val = val * 10 + static_cast<std::size_t>(c - '0');
      }
      cl_declared_ = val;
    } else if (detail::iequals(kv.first, "Transfer-Encoding")) {
      if (detail::iequals(kv.second, "chunked")) {
        has_te_chunked = true;
      }
    }
  }

  if (has_te_chunked) {
    body_mode_ = BodyMode::Chunked;
  } else if (has_cl) {
    body_mode_ = BodyMode::Length;
  } else {
    body_mode_ = BodyMode::None;
  }

  begun_ = true;
}

inline void ResponseSerializer::write_body(std::string_view data) {
  if (error_ != SerializeError::None) return;

  if (ended_) {
    fail(SerializeError::BodyAfterEnd);
    return;
  }

  if (data.empty()) return;  // no-op; avoids premature 0-chunk in chunked mode

  switch (body_mode_) {
    case BodyMode::None:
      fail(SerializeError::WrongBodyMode);
      return;

    case BodyMode::Length:
      // Check for overflow before writing.
      if (cl_written_ + data.size() > cl_declared_) {
        fail(SerializeError::ContentLengthOverflow);
        return;
      }
      buffer_.append(data.data(), data.size());
      cl_written_ += data.size();
      return;

    case BodyMode::Chunked:
      // Emit one chunk: hex-size CRLF data CRLF
      buffer_ += detail::to_hex(data.size());
      buffer_ += "\r\n";
      buffer_.append(data.data(), data.size());
      buffer_ += "\r\n";
      return;
  }
}

inline void ResponseSerializer::end(
    std::vector<std::pair<std::string, std::string>> trailers) {
  if (error_ != SerializeError::None) return;

  if (ended_) return;  // idempotent

  switch (body_mode_) {
    case BodyMode::None:
      // Nothing to finalize.
      break;

    case BodyMode::Length:
      if (cl_written_ != cl_declared_) {
        fail(SerializeError::ContentLengthMismatch);
        return;
      }
      break;

    case BodyMode::Chunked:
      // Emit last-chunk: "0\r\n"
      buffer_ += "0\r\n";
      // Emit trailers if provided.
      for (const auto& kv : trailers) {
        buffer_ += kv.first;
        buffer_ += ": ";
        buffer_ += kv.second;
        buffer_ += "\r\n";
      }
      // Final CRLF after trailer section (or after last-chunk if no trailers).
      buffer_ += "\r\n";
      break;
  }

  ended_ = true;
}

inline std::string_view ResponseSerializer::pending_bytes() const {
  if (read_pos_ >= buffer_.size()) return {};
  return std::string_view(buffer_.data() + read_pos_,
                          buffer_.size() - read_pos_);
}

inline void ResponseSerializer::consume(std::size_t n) {
  if (n == 0) return;
  read_pos_ += n;
  if (read_pos_ > buffer_.size()) {
    read_pos_ = buffer_.size();  // clamp
  }
  // Compact buffer when fully consumed to avoid unbounded growth.
  if (read_pos_ == buffer_.size()) {
    buffer_.clear();
    read_pos_ = 0;
  }
}

inline bool ResponseSerializer::done() const {
  return ended_ && read_pos_ >= buffer_.size();
}

} // namespace core
} // namespace httplib

#endif // CPPHTTPLIB_HTTPLIB_CORE_H
