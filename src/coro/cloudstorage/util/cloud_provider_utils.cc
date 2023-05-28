#include "coro/cloudstorage/util/cloud_provider_utils.h"

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage::util {

namespace {

using Item = AbstractCloudProvider::Item;

Task<Item> GetItemByPathComponents(
    const AbstractCloudProvider* p,
    AbstractCloudProvider::Directory current_directory,
    std::span<const std::string> components, stdx::stop_token stop_token) {
  if (components.empty()) {
    co_return current_directory;
  }
  FOR_CO_AWAIT(auto& page, ListDirectory(p, current_directory, stop_token)) {
    for (auto& item : page.items) {
      auto r = std::visit(
          [&]<typename T>(
              T& d) -> std::variant<std::monostate, Task<Item>, Item> {
            if constexpr (std::is_same_v<T, AbstractCloudProvider::Directory>) {
              if (d.name == components.front()) {
                return GetItemByPathComponents(
                    p, std::move(d), components.subspan(1), stop_token);
              }
            } else {
              if (d.name == components.front()) {
                return std::move(d);
              }
            }
            return std::monostate();
          },
          item);
      if (std::holds_alternative<Task<Item>>(r)) {
        co_return co_await std::get<Task<Item>>(r);
      } else if (std::holds_alternative<Item>(r)) {
        co_return std::move(std::get<Item>(r));
      }
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

Task<Item> GetItemByPath(const AbstractCloudProvider* d,
                         AbstractCloudProvider::Directory current_directory,
                         std::string_view path, stdx::stop_token stop_token) {
  co_return co_await GetItemByPathComponents(
      d, std::move(current_directory), SplitString(std::string(path), '/'),
      std::move(stop_token));
}

std::string Trim(std::string input, http::Range range) {
  if (range.start != 0 || (range.end && *range.end != input.size())) {
    return std::move(input).substr(
        range.start,
        range.end ? *range.end - range.start + 1 : std::string::npos);
  } else {
    return std::move(input);
  }
}

Task<std::string> GenerateThumbnail(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File item,
    stdx::stop_token stop_token) {
  switch (GetFileType(item.mime_type)) {
    case FileType::kImage:
    case FileType::kVideo:
      return (*thumbnail_generator)(
          provider, std::move(item),
          ThumbnailOptions{.codec = ThumbnailOptions::Codec::PNG},
          std::move(stop_token));
    default:
      throw CloudException(CloudException::Type::kNotFound);
  }
}

Task<AbstractCloudProvider::Thumbnail> GetThumbnail(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailQuality quality, http::Range range, stdx::stop_token stop_token) {
  try {
    co_return co_await provider->GetItemThumbnail(file, quality, range,
                                                  stop_token);
  } catch (...) {
  }
  std::string image_bytes = co_await GenerateThumbnail(
      thumbnail_generator, provider, file, std::move(stop_token));
  int64_t size = image_bytes.size();
  co_return AbstractCloudProvider::Thumbnail{
      .data = ToGenerator(Trim(std::move(image_bytes), range)),
      .size = size,
      .mime_type = "image/png"};
}

Task<> UpdateDirectoryListCache(
    const AbstractCloudProvider* provider,
    CloudProviderCacheManager cache_manager,
    std::shared_ptr<
        Promise<std::optional<std::vector<AbstractCloudProvider::Item>>>>
        updated,
    AbstractCloudProvider::Directory directory,
    std::vector<AbstractCloudProvider::Item> previous,
    stdx::stop_token stop_token) {
  try {
    std::vector<AbstractCloudProvider::Item> items;
    std::optional<std::string> page_token;
    do {
      auto page_data = co_await provider->ListDirectoryPage(
          directory, page_token, stop_token);
      std::copy(page_data.items.begin(), page_data.items.end(),
                std::back_inserter(items));
      page_token = std::move(page_data.next_page_token);
    } while (page_token);
    if (!std::equal(items.begin(), items.end(), previous.begin(),
                    previous.end(),
                    [provider](const auto& item1, const auto& item2) {
                      return provider->ToJson(item1) == provider->ToJson(item2);
                    })) {
      co_await cache_manager.Put(directory, items, std::move(stop_token));
      if (updated) {
        updated->SetValue(std::move(items));
      }
    } else {
      if (updated) {
        updated->SetValue(std::nullopt);
      }
    }
  } catch (const std::exception& e) {
    if (updated) {
      updated->SetException(std::current_exception());
    }
  }
}

}  // namespace

FileType GetFileType(std::string_view mime_type) {
  if (mime_type.find("audio") == 0) {
    return FileType::kAudio;
  } else if (mime_type.find("image") == 0) {
    return FileType::kImage;
  } else if (mime_type.find("video") == 0) {
    return FileType::kVideo;
  } else {
    return FileType::kUnknown;
  }
}

Task<Item> GetItemByPathComponents(const AbstractCloudProvider* d,
                                   std::vector<std::string> components,
                                   stdx::stop_token stop_token) {
  co_return co_await GetItemByPathComponents(d, co_await d->GetRoot(stop_token),
                                             std::move(components), stop_token);
}

Task<AbstractCloudProvider::Item> GetItemByPathComponents(
    CloudProviderCacheManager cache_manager,
    std::shared_ptr<Promise<std::optional<AbstractCloudProvider::Item>>>
        updated,
    const AbstractCloudProvider* provider, std::vector<std::string> components,
    stdx::stop_token stop_token) {
  std::optional<AbstractCloudProvider::Item> item =
      co_await cache_manager.Get(components, stop_token);
  if (item) {
    RunTask([cache_manager = std::move(cache_manager),
             updated = std::move(updated), provider, previous_item = *item,
             components =
                 std::vector<std::string>(components.begin(), components.end()),
             stop_token = std::move(stop_token)]() mutable -> Task<> {
      try {
        auto item =
            co_await GetItemByPathComponents(provider, components, stop_token);
        if (provider->ToJson(item) != provider->ToJson(previous_item)) {
          co_await cache_manager.Put(std::move(components), item,
                                     std::move(stop_token));
          if (updated) {
            updated->SetValue(std::move(item));
          }
        } else {
          if (updated) {
            updated->SetValue(std::nullopt);
          }
        }
      } catch (const std::exception& e) {
        if (updated) {
          updated->SetException(std::current_exception());
        }
      }
    });
    co_return *item;
  } else {
    auto new_item =
        co_await GetItemByPathComponents(provider, components, stop_token);
    co_await cache_manager.Put(std::move(components), new_item,
                               std::move(stop_token));
    if (updated) {
      updated->SetException(std::nullopt);
    }
    co_return new_item;
  }
}

Task<Item> GetItemByPath(const AbstractCloudProvider* d, std::string path,
                         stdx::stop_token stop_token) {
  co_return co_await GetItemByPath(d, co_await d->GetRoot(stop_token),
                                   std::move(path), stop_token);
}

Generator<AbstractCloudProvider::PageData> ListDirectory(
    CloudProviderCacheManager cache_manager,
    std::shared_ptr<
        Promise<std::optional<std::vector<AbstractCloudProvider::Item>>>>
        updated,
    const AbstractCloudProvider* provider,
    AbstractCloudProvider::Directory directory, stdx::stop_token stop_token) {
  auto cached = co_await cache_manager.Get(directory, stop_token);
  if (!cached) {
    std::optional<std::string> page_token;
    std::vector<AbstractCloudProvider::Item> items;
    try {
      do {
        auto page_data = co_await provider->ListDirectoryPage(
            directory, page_token, stop_token);
        std::copy(page_data.items.begin(), page_data.items.end(),
                  std::back_inserter(items));
        co_yield page_data;
        page_token = std::move(page_data.next_page_token);
      } while (page_token);
      co_await cache_manager.Put(std::move(directory), std::move(items),
                                 std::move(stop_token));
      if (updated) {
        updated->SetValue(std::nullopt);
      }
    } catch (...) {
      if (updated) {
        updated->SetException(std::current_exception());
      }
      throw;
    }
  } else {
    RunTask(UpdateDirectoryListCache, provider, std::move(cache_manager),
            std::move(updated), std::move(directory), *cached,
            std::move(stop_token));
    co_yield AbstractCloudProvider::PageData{.items = std::move(*cached)};
  }
}

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::File>(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailQuality quality, http::Range range, stdx::stop_token stop_token) {
  return GetThumbnail(thumbnail_generator, provider, std::move(file), quality,
                      range, std::move(stop_token));
}

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::Directory>(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider,
    AbstractCloudProvider::Directory directory, ThumbnailQuality quality,
    http::Range range, stdx::stop_token stop_token) {
  return provider->GetItemThumbnail(std::move(directory), quality, range,
                                    std::move(stop_token));
}

template <typename Item>
Task<AbstractCloudProvider::Thumbnail> GetItemThumbnailWithFallback(
    const ThumbnailGenerator* thumbnail_generator,
    CloudProviderCacheManager cache_manager,
    const AbstractCloudProvider* provider, Item item, ThumbnailQuality quality,
    http::Range range, stdx::stop_token stop_token) {
  std::optional<CacheManager::ImageData> image_data =
      co_await cache_manager.Get(item, quality, stop_token);
  if (image_data) {
    int64_t size = image_data->image_bytes.size();
    std::string data(image_data->image_bytes.begin(),
                     image_data->image_bytes.end());
    co_return AbstractCloudProvider::Thumbnail{
        .data = ToGenerator(Trim(std::move(data), range)),
        .size = size,
        .mime_type = std::move(image_data->mime_type)};
  }
  AbstractCloudProvider::Thumbnail thumbnail =
      co_await GetItemThumbnailWithFallback(thumbnail_generator, provider, item,
                                            quality, http::Range{}, stop_token);
  auto image_bytes = co_await http::GetBody(std::move(thumbnail.data));
  co_await cache_manager.Put(
      std::move(item), quality,
      std::vector<char>(image_bytes.begin(), image_bytes.end()),
      thumbnail.mime_type, std::move(stop_token));
  co_return AbstractCloudProvider::Thumbnail{
      .data = ToGenerator(Trim(std::move(image_bytes), range)),
      .size = thumbnail.size,
      .mime_type = std::move(thumbnail.mime_type)};
}

Task<AbstractCloudProvider::Item> GetItemById(
    const AbstractCloudProvider* provider, std::string id,
    stdx::stop_token stop_token) {
  if (id == kRootId) {
    co_return co_await provider->GetRoot(std::move(stop_token));
  } else {
    co_return co_await provider->GetItem(std::move(id), std::move(stop_token));
  }
}

Task<AbstractCloudProvider::Item> GetItemById(
    const AbstractCloudProvider* provider,
    CloudProviderCacheManager cache_manager,
    std::shared_ptr<Promise<std::optional<AbstractCloudProvider::Item>>>
        updated,
    std::string id, stdx::stop_token stop_token) {
  auto item = co_await cache_manager.Get(id, stop_token);
  if (item) {
    RunTask([provider, cache_manager = std::move(cache_manager),
             updated = std::move(updated), id = std::move(id),
             prev_item = *item,
             stop_token = std::move(stop_token)]() mutable -> Task<> {
      try {
        auto item = co_await GetItemById(provider, id, stop_token);
        if (provider->ToJson(item) != provider->ToJson(prev_item)) {
          co_await cache_manager.Put(std::move(id), item,
                                     std::move(stop_token));
          if (updated) {
            updated->SetValue(std::move(item));
          }
        } else {
          if (updated) {
            updated->SetValue(std::nullopt);
          }
        }
      } catch (const std::exception& e) {
        if (updated) {
          updated->SetException(std::current_exception());
        }
      }
    });
    co_return *item;
  } else {
    auto item = co_await GetItemById(provider, id, stop_token);
    co_await cache_manager.Put(std::move(id), item, std::move(stop_token));
    if (updated) {
      updated->SetValue(std::nullopt);
    }
    co_return item;
  }
}

template Task<AbstractCloudProvider::Thumbnail> GetItemThumbnailWithFallback(
    const ThumbnailGenerator*, CloudProviderCacheManager,
    const AbstractCloudProvider*, AbstractCloudProvider::File, ThumbnailQuality,
    http::Range, stdx::stop_token);

template Task<AbstractCloudProvider::Thumbnail> GetItemThumbnailWithFallback(
    const ThumbnailGenerator*, CloudProviderCacheManager,
    const AbstractCloudProvider*, AbstractCloudProvider::Directory,
    ThumbnailQuality, http::Range, stdx::stop_token);

}  // namespace coro::cloudstorage::util