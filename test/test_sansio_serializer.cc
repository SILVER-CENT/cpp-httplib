//
//  test_sansio_serializer.cc
//
//  Unit tests for httplib::core::ResponseSerializer. These tests drive the
//  serializer purely through push-bytes-out / pull-bytes — no sockets, no
//  Stream.
//

#include "httplib_core.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using httplib::core::ResponseHead;
using httplib::core::ResponseSerializer;
using httplib::core::SerializeError;

namespace {

// Helper: drain all pending_bytes into a string, consuming as we go.
std::string drain(ResponseSerializer& s) {
  std::string out;
  for (;;) {
    auto p = s.pending_bytes();
    if (p.empty()) break;
    out.append(p.data(), p.size());
    s.consume(p.size());
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------
TEST(SansIoResponseSerializer, Simple200_NoBody) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  s.begin(std::move(head));
  s.end();

  auto output = drain(s);
  EXPECT_EQ(output, "HTTP/1.1 200 OK\r\n\r\n");
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

TEST(SansIoResponseSerializer, WithContentLength_SingleWrite) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "11");
  head.headers.emplace_back("Content-Type", "text/plain");
  s.begin(std::move(head));
  s.write_body("hello world");
  s.end();

  auto output = drain(s);
  EXPECT_EQ(output,
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 11\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "hello world");
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

TEST(SansIoResponseSerializer, WithContentLength_MultipleWrites) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "12");
  s.begin(std::move(head));
  s.write_body("Hello");
  s.write_body(" ");
  s.write_body("World!");
  s.end();

  auto output = drain(s);
  // Status-line + header + CRLF + body
  std::string expected =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 12\r\n"
      "\r\n"
      "Hello World!";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

TEST(SansIoResponseSerializer, Status100_Continue) {
  // 100 Continue is a complete HTTP message: status-line + CRLF, no body.
  ResponseSerializer s;
  ResponseHead head;
  head.status = 100;
  s.begin(std::move(head));
  s.end();

  auto output = drain(s);
  EXPECT_EQ(output, "HTTP/1.1 100 Continue\r\n\r\n");
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

TEST(SansIoResponseSerializer, CustomStatusAndVersion) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 404;
  head.version = "HTTP/1.0";
  head.headers.emplace_back("Connection", "close");
  s.begin(std::move(head));
  s.end();

  auto output = drain(s);
  EXPECT_EQ(output,
      "HTTP/1.0 404 Not Found\r\n"
      "Connection: close\r\n"
      "\r\n");
  EXPECT_EQ(s.error(), SerializeError::None);
}

// ---------------------------------------------------------------------------
// Chunked
// ---------------------------------------------------------------------------
TEST(SansIoResponseSerializer, Chunked_SingleChunk) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Transfer-Encoding", "chunked");
  s.begin(std::move(head));
  s.write_body("hello");
  s.end();

  auto output = drain(s);
  std::string expected =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n\r\n";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

TEST(SansIoResponseSerializer, Chunked_MultipleChunks) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Transfer-Encoding", "chunked");
  s.begin(std::move(head));
  s.write_body("Hello");
  s.write_body(" ");
  s.write_body("World!");
  s.end();

  auto output = drain(s);
  std::string expected =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nHello\r\n"
      "1\r\n \r\n"
      "6\r\nWorld!\r\n"
      "0\r\n\r\n";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(s.error(), SerializeError::None);
}

TEST(SansIoResponseSerializer, Chunked_WithTrailers) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Transfer-Encoding", "chunked");
  s.begin(std::move(head));
  s.write_body("data");
  std::vector<std::pair<std::string, std::string>> trailers = {
    {"X-Checksum", "abc123"},
    {"X-Count", "1"},
  };
  s.end(std::move(trailers));

  auto output = drain(s);
  std::string expected =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "X-Checksum: abc123\r\n"
      "X-Count: 1\r\n"
      "\r\n";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(s.error(), SerializeError::None);
}

TEST(SansIoResponseSerializer, Chunked_EmptyBody) {
  // TE: chunked, but no body written. end() emits just the 0-chunk + CRLF.
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Transfer-Encoding", "chunked");
  s.begin(std::move(head));
  s.end();

  auto output = drain(s);
  std::string expected =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "0\r\n\r\n";
  EXPECT_EQ(output, expected);
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

// ---------------------------------------------------------------------------
// Pull-side / consume
// ---------------------------------------------------------------------------
TEST(SansIoResponseSerializer, ConsumePartial_RemainderAvailable) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "5");
  s.begin(std::move(head));
  s.write_body("hello");
  s.end();

  // Read the full pending output.
  auto full = std::string(s.pending_bytes());
  ASSERT_FALSE(full.empty());

  // Consume only half.
  auto half = full.size() / 2;
  s.consume(half);

  // The remainder should still be available.
  auto rest = std::string(s.pending_bytes());
  EXPECT_EQ(rest, full.substr(half));
  EXPECT_FALSE(s.done());

  // Consume the rest.
  s.consume(rest.size());
  EXPECT_TRUE(s.done());
  EXPECT_TRUE(s.pending_bytes().empty());
}

TEST(SansIoResponseSerializer, WriteBeforeConsume_BuffersCorrectly) {
  // Push 3 write_body calls before any consume. All bytes must buffer.
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "15");
  s.begin(std::move(head));
  s.write_body("aaaaa");
  s.write_body("bbbbb");
  s.write_body("ccccc");
  s.end();

  auto output = drain(s);
  // The body part should be "aaaaabbbbbccccc"
  EXPECT_NE(output.find("aaaaabbbbbccccc"), std::string::npos);
  EXPECT_EQ(s.error(), SerializeError::None);
  EXPECT_TRUE(s.done());
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
TEST(SansIoResponseSerializer, Error_StatusOutOfRange) {
  {
    ResponseSerializer s;
    ResponseHead head;
    head.status = 99;
    s.begin(std::move(head));
    EXPECT_EQ(s.error(), SerializeError::StatusOutOfRange);
  }
  {
    ResponseSerializer s;
    ResponseHead head;
    head.status = 1000;
    s.begin(std::move(head));
    EXPECT_EQ(s.error(), SerializeError::StatusOutOfRange);
  }
}

TEST(SansIoResponseSerializer, Error_BodyAfterEnd) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "0");
  s.begin(std::move(head));
  s.end();
  EXPECT_EQ(s.error(), SerializeError::None);

  // Now write after end — should error.
  s.write_body("oops");
  EXPECT_EQ(s.error(), SerializeError::BodyAfterEnd);
}

TEST(SansIoResponseSerializer, Error_ContentLengthMismatch_Under) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "10");
  s.begin(std::move(head));
  s.write_body("short");  // 5 bytes, declared 10
  s.end();
  EXPECT_EQ(s.error(), SerializeError::ContentLengthMismatch);
}

TEST(SansIoResponseSerializer, Error_ContentLengthMismatch_Over) {
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  head.headers.emplace_back("Content-Length", "3");
  s.begin(std::move(head));
  s.write_body("toolong");  // 7 bytes, declared 3
  EXPECT_EQ(s.error(), SerializeError::ContentLengthOverflow);
}

TEST(SansIoResponseSerializer, Error_WrongBodyMode) {
  // No Content-Length, no Transfer-Encoding → body mode None.
  // write_body with data should fail.
  ResponseSerializer s;
  ResponseHead head;
  head.status = 200;
  s.begin(std::move(head));
  s.write_body("data");
  EXPECT_EQ(s.error(), SerializeError::WrongBodyMode);
}

// ---------------------------------------------------------------------------
// Sanity
// ---------------------------------------------------------------------------
TEST(SansIoResponseSerializer, DefaultConstructed_PendingEmpty_NotDone) {
  ResponseSerializer s;
  EXPECT_TRUE(s.pending_bytes().empty());
  EXPECT_FALSE(s.done());
  EXPECT_EQ(s.error(), SerializeError::None);
}

TEST(SansIoResponseSerializer, ConsumeZero_IsNoOp) {
  ResponseSerializer s;
  s.consume(0);
  EXPECT_TRUE(s.pending_bytes().empty());
  EXPECT_FALSE(s.done());
  EXPECT_EQ(s.error(), SerializeError::None);
}
