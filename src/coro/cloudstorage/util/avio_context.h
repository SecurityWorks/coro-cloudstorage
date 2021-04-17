#ifndef CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/util/event_loop.h>

#include <future>

extern "C" {
#include <libavformat/avformat.h>
};

namespace coro::cloudstorage::util {

template <typename EventLoop, typename CloudProvider,
          IsFile<CloudProvider> File>
auto CreateIOContext(EventLoop* event_loop, CloudProvider* provider, File file,
                     stdx::stop_token stop_token) {
  struct Context {
    EventLoop* event_loop;
    CloudProvider* provider;
    File file;
    int64_t offset;
    stdx::stop_token stop_token;
    std::optional<Generator<std::string>> generator;
    std::optional<Generator<std::string>::iterator> it;
  };
  struct AVIOContextDeleter {
    void operator()(AVIOContext* context) {
      delete reinterpret_cast<Context*>(context->opaque);
      av_free(context->buffer);
      avio_context_free(&context);
    }
  };
  const int kBufferSize = 4 * 1024;
  auto buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
  return std::unique_ptr<AVIOContext, AVIOContextDeleter>(avio_alloc_context(
      buffer, kBufferSize, /*write_flag=*/0,
      new Context{event_loop, provider, std::move(file), 0,
                  std::move(stop_token)},
      [](void* opaque, uint8_t* buf, int buf_size) -> int {
        auto data = reinterpret_cast<Context*>(opaque);
        std::promise<int> promise;
        data->event_loop->RunOnEventLoop([&]() -> Task<> {
          try {
            if (!data->generator) {
              data->generator = data->provider->GetFileContent(
                  data->file, http::Range{.start = data->offset},
                  data->stop_token);
              data->it = co_await data->generator->begin();
            }
            auto buffer = co_await http::GetBody(util::Take(
                *data->generator, *data->it, static_cast<size_t>(buf_size)));
            data->offset += buffer.size();
            memcpy(buf, buffer.data(), buffer.size());
            promise.set_value(static_cast<int>(buffer.size()));
          } catch (...) {
            promise.set_value(-1);
          }
        });
        return promise.get_future().get();
      },
      /*write_packet=*/nullptr,
      [](void* opaque, int64_t offset, int whence) -> int64_t {
        auto data = reinterpret_cast<Context*>(opaque);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) {
          return CloudProvider::GetSize(data->file).value_or(-1);
        }
        if (whence == SEEK_SET) {
          data->offset = offset;
        } else if (whence == SEEK_CUR) {
          data->offset += offset;
        } else if (whence == SEEK_END) {
          auto size = CloudProvider::GetSize(data->file);
          if (!size) {
            return -1;
          }
          data->offset = *size + offset;
        } else {
          return -1;
        }
        std::promise<int64_t> promise;
        data->event_loop->RunOnEventLoop([&]() -> Task<> {
          try {
            data->generator = data->provider->GetFileContent(
                data->file, http::Range{.start = data->offset},
                data->stop_token);
            data->it = co_await data->generator->begin();
            promise.set_value(data->offset);
          } catch (...) {
            promise.set_value(-1);
          }
        });
        return promise.get_future().get();
      }));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H
