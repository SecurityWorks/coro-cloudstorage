#include "coro/cloudstorage/test/fake_http_client.h"

#include <fmt/format.h>

#include <iostream>

namespace coro::cloudstorage::test {

namespace {

using Request = http::Request<>;
using Response = http::Response<>;

using ::coro::http::GetBody;

Response RespondToRangeRequestWith(const http::Request<std::string>& request,
                                   std::string_view message) {
  bool has_range_header = false;
  auto range = [&]() -> http::Range {
    if (auto header = http::GetHeader(request.headers, "Range")) {
      has_range_header = true;
      return http::ParseRange(std::move(*header));
    } else {
      return http::Range{};
    }
  }();
  if (!range.end) {
    range.end = message.size();
  }
  Response d{.status = has_range_header ? 206 : 200};
  d.headers.emplace_back("Accept-Ranges", "bytes");
  d.headers.emplace_back("Content-Length",
                         std::to_string(*range.end - range.start + 1));
  if (has_range_header) {
    d.headers.emplace_back(
        "Content-Range",
        fmt::format("bytes {}-{}/{}", range.start, *range.end, message.size()));
  }
  d.body = http::CreateBody(
      std::string(message.substr(range.start, *range.end - range.start + 1)));
  return d;
}

}  // namespace

HttpRequestStubbingBuilder&& HttpRequestStubbingBuilder::WithBody(
    Matcher<std::string> body_matcher) && {
  body_matcher_ = std::move(body_matcher);
  return std::move(*this);
}

HttpRequestStubbing HttpRequestStubbingBuilder::WillReturn(
    std::string_view message) && {
  return std::move(*this).WillReturn(
      ResponseContent{.status = 200, .body = std::string(message)});
}

HttpRequestStubbing HttpRequestStubbingBuilder::WillReturn(
    ResponseContent response) && {
  return HttpRequestStubbing{
      .matcher = std::move(*this).CreateRequestMatcher(),
      .request_f = [response = std::move(response)](
                       const http::Request<std::string>& request) mutable {
        Response d{.status = response.status,
                   .headers = std::move(response.headers)};
        d.headers.emplace_back("Content-Length",
                               std::to_string(response.body.size()));
        d.body = http::CreateBody(std::move(response.body));
        return d;
      }};
}

HttpRequestStubbing HttpRequestStubbingBuilder::WillRespondToRangeRequestWith(
    std::string_view message) && {
  return HttpRequestStubbing{
      .matcher = std::move(*this).CreateRequestMatcher(),
      .request_f =
          [message = std::string(message)](
              const http::Request<std::string>& request) {
            return RespondToRangeRequestWith(request, message);
          },
      .pending = false};
}

stdx::any_invocable<bool(const http::Request<std::string>&) const>
HttpRequestStubbingBuilder::CreateRequestMatcher() && {
  return [url_matcher = std::move(url_matcher_),
          body_matcher = std::move(body_matcher_)](const auto& request) {
    return url_matcher.Matches(request.url) &&
           (!body_matcher || body_matcher->Matches(request.body.value_or("")));
  };
}

HttpRequestStubbingBuilder HttpRequest(Matcher<std::string> url_matcher) {
  return HttpRequestStubbingBuilder(std::move(url_matcher));
}

FakeHttpClient::~FakeHttpClient() {
  for (const auto& stubbing : stubbings_) {
    if (stubbing.pending) {
      std::cerr << "Unsatisfied http request stubbings.\n";
      abort();
    }
  }
}

Task<Response> FakeHttpClient::Fetch(Request request, stdx::stop_token) const {
  std::string body =
      request.body ? co_await GetBody(std::move(*request.body)) : "";
  http::Request<std::string> request_s{.url = std::move(request.url),
                                       .method = request.method,
                                       .headers = std::move(request.headers),
                                       .body = std::move(body)};
  for (auto it = stubbings_.begin(); it != stubbings_.end();) {
    if (it->matcher(request_s)) {
      auto result = it->request_f(std::move(request_s));
      if (it->pending) {
        it = stubbings_.erase(it);
      }
      co_return result;
    } else {
      it++;
    }
  }
  throw http::HttpException(
      500, fmt::format("unexpected request url = {}", request_s.url));
}

FakeHttpClient& FakeHttpClient::Expect(HttpRequestStubbing stubbing) {
  stubbings_.push_back(std::move(stubbing));
  return *this;
}

}  // namespace coro::cloudstorage::test