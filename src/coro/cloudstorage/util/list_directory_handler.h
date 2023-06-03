#ifndef CORO_CLOUDSTORAGE_UTIL_LIST_DIRECTORY_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_LIST_DIRECTORY_HANDLER_H

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/http/http_parse.h"
#include "coro/mutex.h"

namespace coro::cloudstorage::util {

class ListDirectoryHandler {
 public:
  ListDirectoryHandler(
      AbstractCloudProvider* provider, const Clock* clock,
      CloudProviderCacheManager cache_manager,
      stdx::any_invocable<std::string(std::string_view item_id) const>
          list_url_generator,
      stdx::any_invocable<std::string(std::string_view item_id) const>
          thumbnail_url_generator,
      stdx::any_invocable<std::string(std::string_view item_id) const>
          content_url_generator)
      : provider_(provider),
        clock_(clock),
        cache_manager_(std::move(cache_manager)),
        list_url_generator_(std::move(list_url_generator)),
        thumbnail_url_generator_(std::move(thumbnail_url_generator)),
        content_url_generator_(std::move(content_url_generator)) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token);

 private:
  Generator<std::string> GetDirectoryContent(
      std::string host, AbstractCloudProvider::Directory parent,
      Generator<AbstractCloudProvider::PageData> page_data,
      stdx::stop_token stop_token) const;

  AbstractCloudProvider* provider_;
  const Clock* clock_;
  CloudProviderCacheManager cache_manager_;
  stdx::any_invocable<std::string(std::string_view item_id) const>
      list_url_generator_;
  stdx::any_invocable<std::string(std::string_view id) const>
      thumbnail_url_generator_;
  stdx::any_invocable<std::string(std::string_view item_id) const>
      content_url_generator_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_LIST_DIRECTORY_HANDLER_H
