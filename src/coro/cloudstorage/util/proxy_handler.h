#ifndef CORO_CLOUDSTORAGE_PROXY_HANDLER_H
#define CORO_CLOUDSTORAGE_PROXY_HANDLER_H

#include <coro/http/http_parse.h>

namespace coro::cloudstorage::util {

template <typename CloudProvider>
class ProxyHandler {
 public:
  using File = typename CloudProvider::File;
  using Item = typename CloudProvider::Item;
  using Directory = typename CloudProvider::Directory;

  ProxyHandler(CloudProvider provider, std::string path_prefix)
      : provider_(std::move(provider)), path_prefix_(std::move(path_prefix)) {}
  ProxyHandler(ProxyHandler&& handler) = default;

  ProxyHandler(const ProxyHandler&) = delete;
  ProxyHandler& operator=(const ProxyHandler&) = delete;

  ~ProxyHandler() {
    if (shared_data_) {
      shared_data_->stop_source.request_stop();
    }
  }

  Task<http::Response<>> operator()(const coro::http::Request<>& request,
                                    coro::stdx::stop_token stop_token) {
    std::cerr << "[" << request.method << "] " << request.url << " ";
    std::string path =
        http::DecodeUri(http::ParseUri(request.url).path.value_or(""))
            .substr(path_prefix_.length());
    auto it = request.headers.find("Range");
    coro::http::Range range = {};
    if (it != std::end(request.headers)) {
      range = http::ParseRange(it->second);
      std::cerr << it->second;
    }
    std::cerr << "\n";
    auto item = co_await GetItem(path, stop_token);
    if (std::holds_alternative<File>(item)) {
      auto file = std::get<File>(item);
      std::unordered_multimap<std::string, std::string> headers = {
          {"Content-Type", file.mime_type},
          {"Content-Disposition", "inline; filename=\"" + file.name + "\""},
          {"Access-Control-Allow-Origin", "*"},
          {"Access-Control-Allow-Headers", "*"}};
      if (file.size) {
        if (!range.end) {
          range.end = *file.size - 1;
        }
        headers.insert({"Accept-Ranges", "bytes"});
        headers.insert(
            {"Content-Length", std::to_string(*range.end - range.start + 1)});
        if (it != std::end(request.headers)) {
          std::stringstream range_str;
          range_str << "bytes " << range.start << "-" << *range.end << "/"
                    << *file.size;
          headers.insert({"Content-Range", std::move(range_str).str()});
        }
      }
      co_return http::Response<>{
          .status = it == std::end(request.headers) || !file.size ? 200 : 206,
          .headers = std::move(headers),
          .body = provider_.GetFileContent(file, range, stop_token)};
    } else {
      auto directory = std::get<Directory>(item);
      co_return http::Response<>{
          .status = 200,
          .headers = {{"Content-Type", "text/html"}},
          .body = GetDirectoryContent(path, directory, stop_token)};
    }
  }

  Task<Item> GetItem(std::string path, coro::stdx::stop_token stop_token) {
    auto it = shared_data_->tasks.find(path);
    if (it == std::end(shared_data_->tasks)) {
      auto promise = Promise<Item>(
          [path, stop_token = shared_data_->stop_source.get_token(),
           this]() -> Task<Item> {
            co_return co_await provider_.GetItemByPath(path, stop_token);
          });
      it = shared_data_->tasks.insert({path, std::move(promise)}).first;
    }
    co_return co_await it->second.Get(stop_token);
  }

  Generator<std::string> GetDirectoryContent(
      std::string path, Directory directory,
      coro::stdx::stop_token stop_token) {
    co_yield "<!DOCTYPE html>"
             "<html><head><meta charset='UTF-8'></head><body><table>";
    if (!path.empty() && path.back() != '/') {
      path += '/';
    }
    co_yield "<tr><td>[DIR]</td><td><a href='" +
        GetDirectoryPath(path_prefix_ + path) + "'>..</a></td></tr>";
    FOR_CO_AWAIT(
        const auto& page, provider_.ListDirectory(directory, stop_token), {
          for (const auto& item : page.items) {
            auto name = std::visit([](auto item) { return item.name; }, item);
            std::string type =
                std::holds_alternative<Directory>(item) ? "DIR" : "FILE";
            co_yield "<tr><td>[" + type + "]</td><td><a href='" + path_prefix_ +
                path + coro::http::EncodeUri(name) + "'>" + name +
                "</a></td></tr>";
          }
        });
    co_yield "</table></body></html>";
  }

 private:
  static std::string GetDirectoryPath(std::string path) {
    if (path.empty()) {
      return "";
    }
    path.pop_back();
    auto it = path.find_last_of('/');
    if (it == std::string_view::npos) {
      return "";
    }
    return std::string(path.begin(), path.begin() + it + 1);
  }

  CloudProvider provider_;
  std::string path_prefix_;
  struct SharedData {
    std::unordered_map<std::string, Promise<Item>> tasks;
    coro::stdx::stop_source stop_source;
  };
  std::shared_ptr<SharedData> shared_data_ = std::make_shared<SharedData>();
};

template <typename CloudProvider>
auto MakeProxyHandler(CloudProvider cloud_provider, std::string path_prefix) {
  return ProxyHandler<CloudProvider>(std::move(cloud_provider),
                                     std::move(path_prefix));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_PROXY_HANDLER_H