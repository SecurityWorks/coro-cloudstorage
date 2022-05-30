#ifndef CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
#define CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H

#include <string>
#include <string_view>

#include "coro/stdx/concepts.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider>
typename CloudProvider::Auth::AuthData GetAuthData() = delete;

template <typename T>
concept HasRedirectUri = requires(T v) {
                           {
                             v.redirect_uri
                             } -> stdx::convertible_to<std::string>;
                         };

struct AuthData {
  static constexpr std::string_view kHostname = CORO_CLOUDSTORAGE_REDIRECT_URI;

  template <typename CloudProvider>
  auto operator()() const {
    auto auth_data = GetAuthData<CloudProvider>();
    if constexpr (HasRedirectUri<decltype(auth_data)>) {
      auth_data.redirect_uri =
          std::string(kHostname) + "/auth/" + std::string(CloudProvider::kId);
    }
    return auth_data;
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
