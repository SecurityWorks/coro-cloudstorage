#ifndef CORO_CLOUDSTORAGE_YOUTUBE_H
#define CORO_CLOUDSTORAGE_YOUTUBE_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/providers/google_drive.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/task.h"
#include "coro/util/lru_cache.h"

namespace coro::cloudstorage {

struct YouTube {
  using json = nlohmann::json;
  using Request = http::Request<>;

  static inline constexpr int32_t kDashManifestSize = 16192;

  static json GetConfig(std::string_view page_data);
  static std::string GetPlayerUrl(std::string_view page_data);
  static std::string GenerateDashManifest(std::string_view path,
                                          std::string_view name,
                                          const json& stream_data);
  static std::function<std::string(std::string_view cipher)> GetDescrambler(
      std::string_view page);
  static std::optional<std::function<std::string(std::string_view cipher)>>
  GetNewDescrambler(std::string_view page);

  template <http::HttpClient Http>
  static Task<std::string> GetVideoPage(const Http& http, std::string video_id,
                                        stdx::stop_token stop_token) {
    auto response = co_await http.Fetch(
        "https://www.youtube.com/watch?v=" + video_id, stop_token);
    co_return co_await http::GetBody(std::move(response.body));
  }

  struct Auth : GoogleDrive::Auth {
    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://accounts.google.com/o/oauth2/auth?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"redirect_uri", data.redirect_uri},
                                     {"scope",
                                      "https://www.googleapis.com/auth/"
                                      "youtube.readonly openid email"},
                                     {"access_type", "offline"},
                                     {"prompt", "consent"},
                                     {"state", data.state}});
    }
  };

  template <typename AuthManager = class AuthManagerT,
            typename Muxer = class MuxerT>
  struct CloudProvider;

  enum class Presentation { kDash, kStream, kMuxedStreamWebm, kMuxedStreamMp4 };

  struct ItemData {
    std::string id;
    std::string name;
  };

  struct RootDirectory : ItemData {
    Presentation presentation;
  };

  struct StreamDirectory : ItemData {
    std::string video_id;
    int64_t timestamp;
  };

  struct Playlist : ItemData {
    std::string playlist_id;
    Presentation presentation;
  };

  struct MuxedStreamWebm : ItemData {
    static constexpr std::string_view mime_type = "application/octet-stream";
    std::string video_id;
    int64_t timestamp;
    std::optional<std::string> thumbnail_url;
  };

  struct MuxedStreamMp4 : MuxedStreamWebm {};

  struct Stream : ItemData {
    std::string video_id;
    std::string mime_type;
    int64_t size;
    int64_t itag;
  };

  struct DashManifest : ItemData {
    static constexpr std::string_view mime_type = "application/dash+xml";
    static constexpr int64_t size = kDashManifestSize;
    std::string video_id;
    int64_t timestamp;
    std::optional<std::string> thumbnail_url;
  };

  struct StreamData {
    json adaptive_formats;
    json formats;
    std::optional<std::function<std::string(std::string_view)>> descrambler;
    std::optional<std::function<std::string(std::string_view)>> new_descrambler;

    json GetBestVideo(std::string_view mime_type) const;
    json GetBestAudio(std::string_view mime_type) const;
  };

  using Item =
      std::variant<DashManifest, RootDirectory, Stream, MuxedStreamWebm,
                   MuxedStreamMp4, StreamDirectory, Playlist>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  static Stream ToStream(const StreamDirectory&, json d);

  static constexpr std::string_view kId = "youtube";
  static inline constexpr auto& kIcon = util::kAssetsProvidersYoutubePng;
};

template <typename AuthManager, typename Muxer>
struct YouTube::CloudProvider
    : coro::cloudstorage::CloudProvider<YouTube,
                                        CloudProvider<AuthManager, Muxer>> {
  using Request = http::Request<std::string>;

  CloudProvider(AuthManager auth_manager, const Muxer* muxer)
      : auth_manager_(std::move(auth_manager)),
        muxer_(muxer),
        stream_cache_(32, GetStreamData{auth_manager_.GetHttp()}) {}

  Task<RootDirectory> GetRoot(stdx::stop_token) {
    RootDirectory d = {};
    d.id = "/";
    d.presentation = Presentation::kDash;
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    json json = co_await auth_manager_.FetchJson(
        Request{.url = "https://openidconnect.googleapis.com/v1/userinfo"},
        std::move(stop_token));
    GeneralData result{.username = json["email"]};
    co_return result;
  }

  Task<PageData> ListDirectoryPage(StreamDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    StreamData data =
        co_await stream_cache_.Get(directory.video_id, stop_token);
    for (const auto& formats : {data.adaptive_formats, data.formats}) {
      for (const auto& d : formats) {
        if (!d.contains("contentLength")) {
          continue;
        }
        result.items.emplace_back(ToStream(directory, d));
      }
    }
    co_return result;
  }

  Task<PageData> ListDirectoryPage(Playlist directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    std::vector<std::pair<std::string, std::string>> headers{
        {"part", "snippet"},
        {"playlistId", directory.playlist_id},
        {"maxResults", "50"}};
    if (page_token) {
      headers.emplace_back("pageToken", *page_token);
    }
    Request request = {.url = GetEndpoint("/playlistItems") + "?" +
                              http::FormDataToString(headers)};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    for (const auto& item : response["items"]) {
      switch (directory.presentation) {
        case Presentation::kMuxedStreamMp4: {
          result.items.emplace_back(
              ToMuxedStream<MuxedStreamMp4>(directory.id, item));
          break;
        }
        case Presentation::kMuxedStreamWebm: {
          result.items.emplace_back(
              ToMuxedStream<MuxedStreamWebm>(directory.id, item));
          break;
        }
        case Presentation::kStream: {
          StreamDirectory streams;
          streams.video_id = item["snippet"]["resourceId"]["videoId"];
          streams.timestamp =
              http::ParseTime(std::string(item["snippet"]["publishedAt"]));
          streams.name = std::string(item["snippet"]["title"]);
          streams.id = directory.id + http::EncodeUri(streams.name) + "/";
          result.items.emplace_back(std::move(streams));
          break;
        }
        case Presentation::kDash: {
          DashManifest file;
          file.video_id = item["snippet"]["resourceId"]["videoId"];
          file.timestamp =
              http::ParseTime(std::string(item["snippet"]["publishedAt"]));
          file.name = std::string(item["snippet"]["title"]) + ".mpd";
          file.id = directory.id + http::EncodeUri(file.name);
          if (item["snippet"]["thumbnails"].contains("default")) {
            file.thumbnail_url =
                item["snippet"]["thumbnails"]["default"]["url"];
          }
          result.items.emplace_back(std::move(file));
          break;
        }
      }
    }
    if (response.contains("nextPageToken")) {
      result.next_page_token = response["nextPageToken"];
    }
    co_return result;
  }

  Task<PageData> ListDirectoryPage(RootDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    Request request = {
        .url = GetEndpoint("/channels") + "?" +
               http::FormDataToString({{"mine", "true"},
                                       {"part", "contentDetails,snippet"},
                                       {"maxResults", "50"}})};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    for (const auto& [key, value] :
         response["items"][0]["contentDetails"]["relatedPlaylists"].items()) {
      result.items.emplace_back(
          Playlist{{.id = directory.id + key + "/", .name = key},
                   value,
                   directory.presentation});
    }
    if (directory.presentation == Presentation::kDash) {
      result.items.emplace_back(RootDirectory{
          {.id = "/streams/", .name = "streams"}, Presentation::kStream});
      result.items.emplace_back(
          RootDirectory{{.id = "/muxed-webm/", .name = "muxed-webm"},
                        Presentation::kMuxedStreamWebm});
      result.items.emplace_back(
          RootDirectory{{.id = "/muxed-mp4/", .name = "muxed-mp4"},
                        Presentation::kMuxedStreamMp4});
    }
    co_return result;
  }

  Generator<std::string> GetFileContent(Stream file, http::Range range,
                                        stdx::stop_token stop_token) {
    if (!range.end) {
      range.end = file.size - 1;
    }
    const auto kChunkSize = 10'000'000;
    for (int64_t i = range.start; i <= *range.end; i += kChunkSize) {
      http::Range subrange{
          .start = i, .end = std::min<int64_t>(i + kChunkSize - 1, *range.end)};
      FOR_CO_AWAIT(std::string & chunk,
                   GetFileContentImpl(file, subrange, stop_token)) {
        co_yield std::move(chunk);
      }
    }
  }

  Generator<std::string> GetFileContent(MuxedStreamWebm file, http::Range range,
                                        stdx::stop_token stop_token) {
    return GetMuxedFileContent(std::move(file), range, "webm",
                               std::move(stop_token));
  }

  Generator<std::string> GetFileContent(MuxedStreamMp4 file, http::Range range,
                                        stdx::stop_token stop_token) {
    return GetMuxedFileContent(std::move(file), range, "mp4",
                               std::move(stop_token));
  }

  Generator<std::string> GetFileContent(DashManifest file, http::Range range,
                                        stdx::stop_token stop_token) {
    StreamData data =
        co_await stream_cache_.Get(file.video_id, std::move(stop_token));
    auto strip_extension = [](std::string_view str) {
      return std::string(str.substr(0, str.size() - 4));
    };
    std::string dash_manifest = GenerateDashManifest(
        util::StrCat("../streams", strip_extension(file.id), "/"),
        strip_extension(file.name), data.adaptive_formats);
    if ((range.end && range.end >= kDashManifestSize) ||
        range.start >= kDashManifestSize) {
      throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
    }
    dash_manifest.resize(kDashManifestSize, ' ');
    co_yield std::move(dash_manifest)
        .substr(static_cast<size_t>(range.start),
                static_cast<size_t>(range.end.value_or(kDashManifestSize - 1) -
                                    range.start + 1));
  }

  Task<Thumbnail> GetItemThumbnail(DashManifest item, http::Range range,
                                   stdx::stop_token stop_token) {
    return GetItemThumbnailImpl(std::move(item), range, std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(MuxedStreamMp4 item, http::Range range,
                                   stdx::stop_token stop_token) {
    return GetItemThumbnailImpl(std::move(item), range, std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(MuxedStreamWebm item, http::Range range,
                                   stdx::stop_token stop_token) {
    return GetItemThumbnailImpl(std::move(item), range, std::move(stop_token));
  }

 private:
  static constexpr std::string_view kEndpoint =
      "https://www.googleapis.com/youtube/v3";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  template <typename MuxedStream>
  Generator<std::string> GetMuxedFileContent(MuxedStream file,
                                             http::Range range,
                                             std::string_view type,
                                             stdx::stop_token stop_token) {
    if (range.start != 0 || range.end) {
      throw CloudException("partial read unsupported");
    }
    StreamData data = co_await stream_cache_.Get(file.video_id, stop_token);
    Stream video_stream{};
    video_stream.video_id = file.video_id;
    auto best_video = data.GetBestVideo(util::StrCat("video/", type));
    video_stream.itag = best_video["itag"];
    video_stream.size = std::stoll(std::string(best_video["contentLength"]));
    Stream audio_stream{};
    audio_stream.video_id = std::move(file.video_id);
    auto best_audio = data.GetBestAudio(util::StrCat("audio/", type));
    audio_stream.itag = best_audio["itag"];
    audio_stream.size = std::stoll(std::string(best_audio["contentLength"]));
    FOR_CO_AWAIT(
        std::string & chunk,
        (*muxer_)(this, std::move(video_stream), this, std::move(audio_stream),
                  type == "webm" ? util::MediaContainer::kWebm
                                 : util::MediaContainer::kMp4,
                  std::move(stop_token))) {
      co_yield std::move(chunk);
    }
  }

  Generator<std::string> GetFileContentImpl(Stream file, http::Range range,
                                            stdx::stop_token stop_token) {
    std::string video_url =
        co_await GetVideoUrl(file.video_id, file.itag, stop_token);
    Request request{.url = std::move(video_url),
                    .headers = {http::ToRangeHeader(range)}};
    auto response =
        co_await auth_manager_.GetHttp().Fetch(std::move(request), stop_token);
    if (response.status / 100 == 4) {
      stream_cache_.Invalidate(file.video_id);
      video_url = co_await GetVideoUrl(file.video_id, file.itag, stop_token);
      Request retry_request{.url = std::move(video_url),
                            .headers = {http::ToRangeHeader(range)}};
      response = co_await auth_manager_.GetHttp().Fetch(
          std::move(retry_request), stop_token);
    }

    int max_redirect_count = 8;
    while (response.status == 302 && max_redirect_count-- > 0) {
      auto redirect_request = Request{
          .url = coro::http::GetHeader(response.headers, "Location").value(),
          .headers = {http::ToRangeHeader(range)}};
      response = co_await auth_manager_.GetHttp().Fetch(
          std::move(redirect_request), stop_token);
    }
    if (response.status / 100 != 2) {
      throw http::HttpException(response.status);
    }

    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  template <typename Item>
  Task<Thumbnail> GetItemThumbnailImpl(Item item, http::Range range,
                                       stdx::stop_token stop_token) {
    if (!item.thumbnail_url) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    Request request{.url = std::move(*item.thumbnail_url),
                    .headers = {ToRangeHeader(range)}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
    Thumbnail result;
    result.mime_type =
        http::GetHeader(response.headers, "Content-Type").value();
    result.size =
        std::stoll(http::GetHeader(response.headers, "Content-Length").value());
    result.data = std::move(response.body);
    co_return result;
  }

  template <typename MuxedStreamT>
  static MuxedStreamT ToMuxedStream(std::string_view directory_id,
                                    nlohmann::json item) {
    MuxedStreamT stream;
    stream.video_id = item["snippet"]["resourceId"]["videoId"];
    stream.timestamp =
        http::ParseTime(std::string(item["snippet"]["publishedAt"]));
    stream.name = util::StrCat(std::string(item["snippet"]["title"]), [] {
      if constexpr (std::is_same_v<MuxedStreamT, MuxedStreamMp4>) {
        return ".mp4";
      } else {
        return ".webm";
      }
    }());
    stream.id = util::StrCat(directory_id, http::EncodeUri(stream.name));
    if (item["snippet"]["thumbnails"].contains("default")) {
      stream.thumbnail_url = item["snippet"]["thumbnails"]["default"]["url"];
    }
    return stream;
  }

  Task<std::string> GetVideoUrl(std::string video_id, int64_t itag,
                                stdx::stop_token stop_token) const {
    StreamData data = co_await stream_cache_.Get(video_id, stop_token);
    std::optional<std::string> url;
    for (const auto& formats : {data.adaptive_formats, data.formats}) {
      for (const auto& d : formats) {
        if (d["itag"] == itag) {
          if (d.contains("url")) {
            url = d["url"];
          } else {
            url = (*data.descrambler)(std::string(d["signatureCipher"]));
          }
          auto uri = http::ParseUri(*url);
          auto params = http::ParseQuery(uri.query.value_or(""));
          if (auto it = params.find("n");
              it != params.end() && data.new_descrambler) {
            it->second = (*data.new_descrambler)(it->second);
            url = util::StrCat(uri.scheme.value_or("https"), "://",
                               uri.host.value_or(""), uri.path.value_or(""),
                               "?", http::FormDataToString(params));
          }
        }
      }
    }
    if (!url) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    co_return *url;
  }

  struct GetStreamData {
    Task<StreamData> operator()(std::string video_id,
                                stdx::stop_token stop_token) const {
      std::string page =
          co_await GetVideoPage(http, std::move(video_id), stop_token);
      json config = GetConfig(page);
      StreamData result{
          .adaptive_formats = config["streamingData"]["adaptiveFormats"],
          .formats = config["streamingData"]["formats"]};
      auto response = co_await http.Fetch(GetPlayerUrl(page), stop_token);
      auto player_content = co_await http::GetBody(std::move(response.body));
      result.new_descrambler = GetNewDescrambler(player_content);
      for (const auto& formats : {result.adaptive_formats, result.formats}) {
        for (const auto& d : formats) {
          if (!d.contains("url")) {
            result.descrambler = GetDescrambler(player_content);
            co_return result;
          }
        }
      }
      co_return result;
    }
    const typename AuthManager::Http& http;
  };

  AuthManager auth_manager_;
  const Muxer* muxer_;
  mutable coro::util::LRUCache<std::string, GetStreamData> stream_cache_;
};

namespace util {
template <>
inline YouTube::Auth::AuthData GetAuthData<YouTube>() {
  return {
      .client_id =
          R"(379556609343-0v8r2fpijkjpj707a76no2rve6nto2co.apps.googleusercontent.com)",
      .client_secret = "_VUpM5Pf9_54RIZq2GGUbUMZ"};
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YOUTUBE_H