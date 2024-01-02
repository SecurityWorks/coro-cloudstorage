#ifndef CORO_CLOUDSTORAGE_FAKE_HTTP_CLIENT_H
#define CORO_CLOUDSTORAGE_FAKE_HTTP_CLIENT_H

#include <coro/http/http.h>
#include <coro/stdx/any_invocable.h>
#include <gtest/gtest.h>

#include <string>

namespace coro::cloudstorage::test {

struct ResponseContent {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Content-Type", "application/x-octet-stream"}};
  std::string body;
};

struct HttpRequestStubbing {
  stdx::any_invocable<bool(const http::Request<std::string>&) const> matcher;
  stdx::any_invocable<http::Response<>(http::Request<std::string> request)>
      request_f;
  bool pending = true;
};

class HttpRequestStubbingBuilder {
 public:
  explicit HttpRequestStubbingBuilder(testing::Matcher<std::string> url_matcher)
      : url_matcher_(std::move(url_matcher)) {}

  HttpRequestStubbingBuilder&& WithBody(
      testing::Matcher<std::string> body_matcher) &&;

  HttpRequestStubbing WillReturn(std::string_view message) &&;

  HttpRequestStubbing WillReturn(ResponseContent response) &&;

  HttpRequestStubbing WillRespondToRangeRequestWith(
      std::string_view message) &&;

 private:
  stdx::any_invocable<bool(const http::Request<std::string>&) const>
  CreateRequestMatcher() &&;

  testing::Matcher<std::string> url_matcher_;
  std::optional<testing::Matcher<std::string>> body_matcher_;
};

HttpRequestStubbingBuilder HttpRequest(
    testing::Matcher<std::string> url_matcher);

class FakeHttpClient {
 public:
  FakeHttpClient() = default;
  FakeHttpClient(const FakeHttpClient&) = default;
  FakeHttpClient(FakeHttpClient&&) = default;
  FakeHttpClient& operator=(const FakeHttpClient&) = default;
  FakeHttpClient& operator=(FakeHttpClient&&) = default;

  ~FakeHttpClient();

  Task<http::Response<>> Fetch(http::Request<> request, stdx::stop_token) const;

  FakeHttpClient& Expect(HttpRequestStubbing stubbing);

 private:
  mutable std::vector<HttpRequestStubbing> stubbings_;
};

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_FAKE_HTTP_CLIENT_H
