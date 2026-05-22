//
//  test_sansio_parser.cc
//
//  Unit tests for httplib::core::RequestParser. These tests drive the
//  parser purely through bytes-in / events-out — no sockets, no Stream.
//
//  Phase 1 scaffold: every test name from PLAN.md is declared here and
//  GTEST_SKIP()'d. As the state machine is implemented (PLAN.md steps
//  5-8), the matching tests get filled in and un-skipped.
//

#include "httplib_core.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using httplib::core::Event;
using httplib::core::Limits;
using httplib::core::ParseError;
using httplib::core::RequestHead;
using httplib::core::RequestParser;

namespace {

// Helper: feed a complete byte stream one byte at a time, polling between
// every feed(). Returns the sequence of events emitted. Bails out on any
// terminal event (Failed / MessageComplete) so callers can assert on the
// final state.
std::vector<Event> drive_byte_by_byte(RequestParser &p, std::string_view s) {
  std::vector<Event> events;
  for (std::size_t i = 0; i < s.size(); ++i) {
    p.feed(s.data() + i, 1);
    for (;;) {
      auto ev = p.poll();
      if (ev == Event::NeedData) break;
      events.push_back(ev);
      if (ev == Event::Failed || ev == Event::MessageComplete) return events;
    }
  }
  return events;
}

// Helper: feed in a single call and drain events until NeedData or terminal.
std::vector<Event> drive_one_shot(RequestParser &p, std::string_view s) {
  p.feed(s.data(), s.size());
  std::vector<Event> events;
  for (;;) {
    auto ev = p.poll();
    if (ev == Event::NeedData) break;
    events.push_back(ev);
    if (ev == Event::Failed || ev == Event::MessageComplete) return events;
  }
  return events;
}

}  // namespace

// ---------------------------------------------------------------------------
// Happy path — feed an entire well-formed request in one call, then walk
// the event sequence. The simplest possible smoke test.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, HappyPath_SingleChunk_GetNoBody) {
  RequestParser p;
  auto events = drive_one_shot(p,
      "GET /hello HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "User-Agent: tester\r\n"
      "\r\n");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], Event::HeadCompleted);
  EXPECT_EQ(events[1], Event::MessageComplete);
  EXPECT_EQ(p.error(), ParseError::None);
  EXPECT_EQ(p.head().method, "GET");
  EXPECT_EQ(p.head().target, "/hello");
  EXPECT_EQ(p.head().version, "HTTP/1.1");
  ASSERT_EQ(p.head().headers.size(), 2u);
  EXPECT_EQ(p.head().headers[0].first, "Host");
  EXPECT_EQ(p.head().headers[0].second, "example.com");
  EXPECT_EQ(p.head().headers[1].first, "User-Agent");
  EXPECT_EQ(p.head().headers[1].second, "tester");
}

TEST(SansIoRequestParser, HappyPath_PostContentLength) {
  RequestParser p;
  const std::string req =
      "POST /api HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 11\r\n"
      "\r\n"
      "hello world";
  p.feed(req.data(), req.size());
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().method, "POST");
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "hello world");
  EXPECT_EQ(p.poll(), Event::MessageComplete);
}

// ---------------------------------------------------------------------------
// Adversarial chunkings — same byte stream, fed in pathological pieces.
// These prove the parser is genuinely byte-fed, not line-fed-in-disguise.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, Chunking_OneByteAtATime_Get) {
  RequestParser p;
  std::string_view req =
      "GET / HTTP/1.1\r\n"
      "Host: x\r\n"
      "\r\n";
  auto events = drive_byte_by_byte(p, req);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], Event::HeadCompleted);
  EXPECT_EQ(events[1], Event::MessageComplete);
  EXPECT_EQ(p.head().method, "GET");
  EXPECT_EQ(p.head().target, "/");
  EXPECT_EQ(p.head().version, "HTTP/1.1");
}

TEST(SansIoRequestParser, Chunking_SplitMidMethod) {
  RequestParser p;
  p.feed("PO", 2);
  EXPECT_EQ(p.poll(), Event::NeedData);
  p.feed("ST /upload HTTP/1.1\r\nHost: x\r\n\r\n", 32);
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().method, "POST");
  EXPECT_EQ(p.head().target, "/upload");
}

TEST(SansIoRequestParser, Chunking_SplitMidHeaderLine) {
  RequestParser p;
  p.feed("GET / HTTP/1.1\r\nHo", 18);
  EXPECT_EQ(p.poll(), Event::NeedData);
  p.feed("st: example.com\r\n\r\n", 19);
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().headers.size(), 1u);
  EXPECT_EQ(p.head().headers[0].first, "Host");
  EXPECT_EQ(p.head().headers[0].second, "example.com");
}

TEST(SansIoRequestParser, Chunking_SplitInsideCRLF) {
  // Split a request such that the \r and \n of a CRLF arrive in separate
  // feeds. The parser must not mistake the lone \r for end-of-line.
  RequestParser p;
  p.feed("GET / HTTP/1.1\r", 15);
  EXPECT_EQ(p.poll(), Event::NeedData);
  p.feed("\nHost: x\r\n\r\n", 12);
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().method, "GET");
}

TEST(SansIoRequestParser, Chunking_SplitInsideChunkSize) {
  RequestParser p;
  const std::string head_and_first_digit =
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "1";  // leading digit of "1a" (= 26)
  p.feed(head_and_first_digit.data(), head_and_first_digit.size());
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.poll(), Event::NeedData);
  // Finish the chunk-size and provide a slice of the chunk.
  const std::string rest_of_size = "a\r\n";
  const std::string first_data = "abcdefghij";  // 10 bytes
  p.feed(rest_of_size.data(), rest_of_size.size());
  p.feed(first_data.data(), first_data.size());
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "abcdefghij");
  // Provide the remaining 16 bytes of the 26-byte chunk, then CRLF + terminator.
  const std::string tail = "klmnopqrstuvwxyz\r\n0\r\n\r\n";
  p.feed(tail.data(), tail.size());
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "klmnopqrstuvwxyz");
  EXPECT_EQ(p.poll(), Event::MessageComplete);
}

TEST(SansIoRequestParser, Chunking_SplitBetweenChunks) {
  RequestParser p;
  const std::string head =
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  p.feed(head.data(), head.size());
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.poll(), Event::NeedData);

  // First chunk arrives in one piece.
  const std::string c1 = "5\r\nhello\r\n";
  p.feed(c1.data(), c1.size());
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "hello");
  // Gap — caller hasn't received the next chunk yet.
  EXPECT_EQ(p.poll(), Event::NeedData);

  // Second chunk + terminator.
  const std::string c2 = "6\r\nworld!\r\n0\r\n\r\n";
  p.feed(c2.data(), c2.size());
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "world!");
  EXPECT_EQ(p.poll(), Event::MessageComplete);
}

// ---------------------------------------------------------------------------
// Error paths — every ParseError value should be reachable from a specific
// well-defined bad input.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, Error_BadMethod) {
  RequestParser p;
  // "FETCH" is a valid token but not in the accepted method whitelist —
  // matches the existing Server::parse_request_line behaviour.
  auto events = drive_one_shot(p, "FETCH / HTTP/1.1\r\n\r\n");
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::InvalidHTTPMethod);
}

TEST(SansIoRequestParser, Error_BadHttpVersion) {
  RequestParser p;
  auto events = drive_one_shot(p, "GET / HTTP/2.0\r\n\r\n");
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::InvalidHTTPVersion);
}

TEST(SansIoRequestParser, Error_OversizeHeader) {
  Limits lim;
  lim.max_header_length = 32;
  RequestParser p(lim);
  // Header line longer than the cap.
  std::string req = "GET / HTTP/1.1\r\nX-Big: ";
  req.append(64, 'A');
  req.append("\r\n\r\n");
  auto events = drive_one_shot(p, req);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::ExceedHeaderMaxLength);
}

TEST(SansIoRequestParser, Error_OversizeUri) {
  Limits lim;
  lim.max_uri_length = 16;
  RequestParser p(lim);
  std::string req = "GET /";
  req.append(64, 'a');
  req.append(" HTTP/1.1\r\n\r\n");
  auto events = drive_one_shot(p, req);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::ExceedUriMaxLength);
}

TEST(SansIoRequestParser, Error_OversizeBody) {
  Limits lim;
  lim.max_payload_length = 4;
  RequestParser p(lim);
  auto events = drive_one_shot(p,
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 100\r\n"
      "\r\n");
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::ExceedMaxPayloadSize);
}

// RFC 9112 §6.3 / smuggling defense: both Content-Length and a
// Transfer-Encoding header on the same request must be rejected.
TEST(SansIoRequestParser, Error_ContentLengthAndTransferEncoding) {
  RequestParser p;
  auto events = drive_one_shot(p,
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Length: 5\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n");
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::ContentLengthAndTransferEncoding);
}

TEST(SansIoRequestParser, Error_TooManyHeaders) {
  Limits lim;
  lim.max_header_count = 3;
  RequestParser p(lim);
  std::string req = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 5; ++i) {
    req += "X-H" + std::to_string(i) + ": v\r\n";
  }
  req += "\r\n";
  auto events = drive_one_shot(p, req);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::ExceedHeaderMaxCount);
}

// ---------------------------------------------------------------------------
// Keep-alive — two requests on one connection, reset() between them.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, KeepAlive_TwoRequestsBackToBack) {
  RequestParser p;

  const std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\n\r\n";
  p.feed(req1.data(), req1.size());
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().target, "/one");
  EXPECT_EQ(p.poll(), Event::MessageComplete);

  p.reset();

  const std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\n\r\n";
  p.feed(req2.data(), req2.size());
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().target, "/two");
  EXPECT_EQ(p.poll(), Event::MessageComplete);
}

TEST(SansIoRequestParser, KeepAlive_PipelinedBytesPreservedAcrossReset) {
  // Both requests fed in a single feed() call. After the first completes,
  // reset() must preserve the leftover bytes belonging to the second.
  RequestParser p;
  const std::string pipelined =
      "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
      "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
  p.feed(pipelined.data(), pipelined.size());

  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().target, "/one");
  EXPECT_EQ(p.poll(), Event::MessageComplete);

  p.reset();

  // Without any additional feed(), the second request's bytes are already
  // sitting in the buffer.
  EXPECT_EQ(p.poll(), Event::HeadCompleted);
  EXPECT_EQ(p.head().method, "POST");
  EXPECT_EQ(p.head().target, "/two");
  EXPECT_EQ(p.poll(), Event::BodyChunk);
  EXPECT_EQ(p.body_chunk(), "abc");
  EXPECT_EQ(p.poll(), Event::MessageComplete);
}

// ---------------------------------------------------------------------------
// Chunked-body specifics.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, Chunked_SingleChunk) {
  RequestParser p;
  auto events = drive_one_shot(p,
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n"
      "\r\n");
  // Expect: HeadCompleted, one BodyChunk("hello"), MessageComplete.
  ASSERT_GE(events.size(), 3u);
  EXPECT_EQ(events[0], Event::HeadCompleted);
  EXPECT_EQ(events[1], Event::BodyChunk);
  EXPECT_EQ(events.back(), Event::MessageComplete);
}

TEST(SansIoRequestParser, Chunked_ManySmallChunks) {
  RequestParser p;
  std::string body;
  for (int i = 1; i <= 4; ++i) {
    body += "1\r\n";
    body += static_cast<char>('A' + i - 1);
    body += "\r\n";
  }
  body += "0\r\n\r\n";
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + body;
  auto events = drive_one_shot(p, req);

  // Collect just the body chunks' contents by walking again with a fresh parser.
  RequestParser q;
  std::string acc;
  q.feed(req.data(), req.size());
  for (;;) {
    auto ev = q.poll();
    if (ev == Event::BodyChunk) acc.append(q.body_chunk());
    if (ev == Event::MessageComplete || ev == Event::Failed || ev == Event::NeedData) break;
  }
  EXPECT_EQ(acc, "ABCD");
  EXPECT_EQ(events.back(), Event::MessageComplete);
}

TEST(SansIoRequestParser, Chunked_WithChunkExtensions) {
  RequestParser p;
  auto events = drive_one_shot(p,
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5;ignored=value\r\nhello\r\n"
      "0\r\n"
      "\r\n");
  ASSERT_GE(events.size(), 3u);
  EXPECT_EQ(events[0], Event::HeadCompleted);
  EXPECT_EQ(events[1], Event::BodyChunk);
  EXPECT_EQ(events.back(), Event::MessageComplete);
}

TEST(SansIoRequestParser, Chunked_WithTrailers) {
  RequestParser p;
  auto events = drive_one_shot(p,
      "POST / HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n"
      "X-Trailer-A: one\r\n"
      "X-Trailer-B: two\r\n"
      "\r\n");
  EXPECT_EQ(events.back(), Event::MessageComplete);
  ASSERT_EQ(p.head().trailers.size(), 2u);
  EXPECT_EQ(p.head().trailers[0].first, "X-Trailer-A");
  EXPECT_EQ(p.head().trailers[0].second, "one");
  EXPECT_EQ(p.head().trailers[1].first, "X-Trailer-B");
  EXPECT_EQ(p.head().trailers[1].second, "two");
}

// ---------------------------------------------------------------------------
// Body-without-length — RFC 9112 §6.3 says request bodies require a
// Content-Length or Transfer-Encoding header. No length indicator means
// zero-length body, NOT read-until-EOF. (Response-side parsing is the one
// that needs close-delimited bodies; that lives in phase 4.) These tests
// pin down that behaviour.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, BodyWithoutLength_FeedEofTerminates) {
  // A request with no body framing completes immediately after the head.
  // Subsequent feed_eof() is harmless and the parser stays Complete.
  RequestParser p;
  auto events = drive_one_shot(p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], Event::HeadCompleted);
  EXPECT_EQ(events[1], Event::MessageComplete);
  p.feed_eof();
  EXPECT_EQ(p.poll(), Event::MessageComplete);  // sticky after Complete
  EXPECT_EQ(p.error(), ParseError::None);
}

TEST(SansIoRequestParser, BodyWithoutLength_EofBeforeHeadIsError) {
  RequestParser p;
  p.feed("GET / HTTP/", 11);     // partial; never completes
  p.feed_eof();
  EXPECT_EQ(p.poll(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::UnexpectedEof);
}

// ---------------------------------------------------------------------------
// Final adversarial sweep: a complete chunked POST with trailers, fed one
// byte at a time. Reassembles the body and verifies bit-for-bit equality.
// This exercises every state transition under maximum fragmentation.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, Adversarial_ChunkedOneByteAtATime) {
  const std::string req =
      "POST /sink HTTP/1.1\r\n"
      "Host: x\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nHello\r\n"
      "1\r\n \r\n"
      "6\r\nworld!\r\n"
      "0\r\n"
      "X-Done: yes\r\n"
      "\r\n";
  const std::string expected_body = "Hello world!";

  RequestParser p;
  std::string assembled;
  bool head_seen = false;
  bool complete = false;

  for (std::size_t i = 0; i < req.size() && !complete; ++i) {
    p.feed(req.data() + i, 1);
    for (;;) {
      auto ev = p.poll();
      if (ev == Event::NeedData) break;
      if (ev == Event::HeadCompleted) { head_seen = true; continue; }
      if (ev == Event::BodyChunk) { assembled.append(p.body_chunk()); continue; }
      if (ev == Event::MessageComplete) { complete = true; break; }
      ASSERT_NE(ev, Event::Failed) << "unexpected Failed; error=" << static_cast<int>(p.error());
    }
  }
  EXPECT_TRUE(head_seen);
  EXPECT_TRUE(complete);
  EXPECT_EQ(assembled, expected_body);
  ASSERT_EQ(p.head().trailers.size(), 1u);
  EXPECT_EQ(p.head().trailers[0].first, "X-Done");
  EXPECT_EQ(p.head().trailers[0].second, "yes");
}

// ---------------------------------------------------------------------------
// Sanity: a default-constructed parser produces NeedData and nothing else.
// This one we CAN run today against the skeleton — it exercises the public
// API surface that's already wired up.
// ---------------------------------------------------------------------------
TEST(SansIoRequestParser, Skeleton_EmptyParserReportsNeedData) {
  RequestParser p;
  EXPECT_EQ(p.poll(), Event::NeedData);
  EXPECT_EQ(p.error(), ParseError::None);
}

TEST(SansIoRequestParser, Skeleton_FeedingBytesDoesNotCrash) {
  RequestParser p;
  const char buf[] = "GET / HTTP/1.1\r\n\r\n";
  p.feed(buf, sizeof(buf) - 1);
  // Skeleton can't parse yet, but feed() + poll() must not crash and
  // must not spuriously report Failed.
  EXPECT_NE(p.poll(), Event::Failed);
  EXPECT_EQ(p.error(), ParseError::None);
}

TEST(SansIoRequestParser, Skeleton_ResetClearsState) {
  RequestParser p;
  p.feed("garbage", 7);
  p.reset();
  EXPECT_EQ(p.poll(), Event::NeedData);
  EXPECT_EQ(p.error(), ParseError::None);
}
