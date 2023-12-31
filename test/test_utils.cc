#include "test_utils.h"

#include <fmt/format.h>

#include <fstream>

#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/raii_utils.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
}

namespace coro::cloudstorage::util {

namespace {

constexpr double kEps = 0.0001;

using ::coro::util::AtScopeExit;

struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

struct AVFrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

class TemporaryFile {
 public:
  TemporaryFile() {
#ifdef _WIN32
#else
    std::string tmpl = StrCat(kTestRunDirectory, "/tmp.XXXXX");
    int fd = mkstemp(tmpl.data());
    if (fd < 0) {
      throw RuntimeError("mkstemp error");
    }
    path_ = std::move(tmpl);
    file_.reset(fdopen(fd, "wb+"));
#endif
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile(TemporaryFile&&) = default;
  TemporaryFile& operator=(const TemporaryFile&) = delete;
  TemporaryFile& operator=(TemporaryFile&&) = delete;

  ~TemporaryFile() {
    if (file_) {
      std::remove(path_.c_str());
    }
  }

  std::FILE* stream() const { return file_.get(); }
  std::string_view path() const { return path_; }

 private:
  std::string path_;
  std::unique_ptr<std::FILE, FileDeleter> file_;
};

std::string GetFileContent(std::string_view path) {
  std::ifstream stream(std::string(path), std::fstream::binary);
  std::string data;
  std::string buffer(4096, 0);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    data += std::string_view(buffer.data(), stream.gcount());
  }
  return data;
}

void WriteFileContent(std::string_view path, std::string_view content) {
  std::ofstream stream(std::string(path), std::fstream::binary);
  if (!stream) {
    throw std::runtime_error("File not writeable.");
  }
  stream << content;
}

bool AreVideosEquivImpl(std::string_view path1, std::string_view path2,
                        std::string_view format) {
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph{
      avfilter_graph_alloc()};
  if (!graph) {
    throw RuntimeError("avfilter_graph_alloc");
  }
  AVFilterInOut* inputs = nullptr;
  AVFilterInOut* outputs = nullptr;
  if (avfilter_graph_parse2(
          graph.get(),
          fmt::format("movie=filename={}:f={format} [i1];"
                      "movie=filename={}:f={format} [i2];"
                      "[i1][i2] identity, buffersink@output",
                      path1, path2, fmt::arg("format", format))
              .c_str(),
          &inputs, &outputs) != 0) {
    throw RuntimeError("avfilter_graph_parse2 error");
  }
  auto at_exit = AtScopeExit([&] {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
  });

  if (avfilter_graph_config(graph.get(), nullptr) != 0) {
    throw RuntimeError("avfilter_graph_config error");
  }

  AVFilterContext* sink =
      avfilter_graph_get_filter(graph.get(), "buffersink@output");
  while (true) {
    std::unique_ptr<AVFrame, AVFrameDeleter> frame{av_frame_alloc()};
    if (!frame) {
      throw RuntimeError("av_frame_alloc");
    }
    int err = av_buffersink_get_frame(sink, frame.get());
    if (err == AVERROR_EOF) {
      break;
    } else if (err != 0) {
      throw RuntimeError("av_buffersink_get_frame");
    }
    const AVDictionaryEntry* entry =
        av_dict_get(frame->metadata, "lavfi.identity.identity_avg", nullptr, 0);
    if (entry == nullptr) {
      throw RuntimeError("lavfi.identity.identity_avg attribute missing");
    }
    if (std::abs(std::stod(entry->value) - 1.0) > kEps) {
      return false;
    }
  }

  return true;
}

void WriteFileContent(std::FILE* file, std::string_view content) {
  if (std::fwrite(content.data(), 1, content.size(), file) != content.size()) {
    throw RuntimeError("fwrite error");
  }
  std::fflush(file);
}

}  // namespace

std::string GetTestFileContent(std::string_view filename) {
  return GetFileContent(StrCat(kTestDataDirectory, '/', filename));
}

void WriteTestFileContent(std::string_view filename, std::string_view content) {
  WriteFileContent(StrCat(kTestDataDirectory, '/', filename), content);
}

bool AreVideosEquiv(std::string_view video1, std::string_view video2, std::string_view format) {
  TemporaryFile f1;
  TemporaryFile f2;
  WriteFileContent(f1.stream(), video1);
  WriteFileContent(f2.stream(), video2);
  return AreVideosEquivImpl(f1.path(), f2.path(), format);
}

}  // namespace coro::cloudstorage::util