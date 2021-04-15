#include "thumbnail_generator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace coro::cloudstorage::util {

namespace {

struct ImageSize {
  int width;
  int height;
};

struct AVCodecContextDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct AVFormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct AVPacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct AVFrameDeleter {
  void operator()(AVFrame* frame) const {
    // av_freep(&frame->data);
    av_frame_free(&frame);
  }
};

struct AVFrameConvertedDeleter {
  void operator()(AVFrame* frame) const {
    av_freep(&frame->data);
    av_frame_free(&frame);
  }
};

struct SwsContextDeleter {
  void operator()(SwsContext* context) const { sws_freeContext(context); }
};

struct AVFilterContextDeleter {
  void operator()(AVFilterContext* filter) const { avfilter_free(filter); }
};

struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

std::string av_error(int err) {
  char buffer[AV_ERROR_MAX_STRING_SIZE + 1] = {};
  if (av_strerror(err, buffer, AV_ERROR_MAX_STRING_SIZE) < 0)
    return "invalid error";
  else
    return buffer;
}

void Check(int code, const std::string& call) {
  if (code < 0) {
    throw std::logic_error(call + " (" + av_error(code) + ")");
  }
}

auto CreateFormatContext(AVIOContext* io_context) {
  auto context = avformat_alloc_context();
  context->interrupt_callback.opaque = nullptr;
  context->interrupt_callback.callback = [](void* t) -> int { return 0; };
  context->pb = io_context;
  int e = 0;
  if ((e = avformat_open_input(&context, nullptr, nullptr, nullptr)) < 0) {
    avformat_free_context(context);
    Check(e, "avformat_open_input");
  } else if ((e = avformat_find_stream_info(context, nullptr)) < 0) {
    avformat_close_input(&context);
    Check(e, "avformat_find_stream_info");
  }
  return std::unique_ptr<AVFormatContext, AVFormatContextDeleter>(context);
}

std::unique_ptr<AVCodecContext, AVCodecContextDeleter> CreateCodecContext(
    AVFormatContext* context, int stream_index) {
  auto codec =
      avcodec_find_decoder(context->streams[stream_index]->codecpar->codec_id);
  if (!codec) {
    throw std::logic_error("decoder not found");
  }
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context(
      avcodec_alloc_context3(codec));
  Check(avcodec_parameters_to_context(codec_context.get(),
                                      context->streams[stream_index]->codecpar),
        "avcodec_parameters_to_context");
  Check(avcodec_open2(codec_context.get(), codec, nullptr), "avcodec_open2");
  return codec_context;
}

auto CreatePacket() {
  return std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

auto DecodeFrame(AVFormatContext* context, AVCodecContext* codec_context,
                 int stream_index) {
  std::unique_ptr<AVFrame, AVFrameDeleter> result_frame;
  while (!result_frame) {
    auto packet = CreatePacket();
    auto read_packet = av_read_frame(context, packet.get());
    if (read_packet != 0 && read_packet != AVERROR_EOF) {
      Check(read_packet, "av_read_frame");
    } else {
      if (read_packet == 0 && packet->stream_index != stream_index) {
        continue;
      }
      auto send_packet = avcodec_send_packet(
          codec_context, read_packet == AVERROR_EOF ? nullptr : packet.get());
      if (send_packet != AVERROR_EOF) {
        Check(send_packet, "avcodec_send_packet");
      }
    }
    std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
    auto code = avcodec_receive_frame(codec_context, frame.get());
    if (code == 0) {
      result_frame = std::move(frame);
    } else if (code == AVERROR_EOF) {
      break;
    } else if (code != AVERROR(EAGAIN)) {
      Check(code, "avcodec_receive_frame");
    }
  }
  return result_frame;
}

ImageSize GetThumbnailSize(ImageSize i, int target) {
  if (i.width == 0 || i.height == 0) {
    return {target, target};
  }
  if (i.width > i.height) {
    return {target, i.height * target / i.width};
  } else {
    return {i.width * target / i.height, target};
  }
}

auto ConvertFrame(AVFrame* frame, ImageSize size, AVPixelFormat format) {
  std::unique_ptr<SwsContext, SwsContextDeleter> sws_context(sws_getContext(
      frame->width, frame->height, AVPixelFormat(frame->format), size.width,
      size.height, format, SWS_BICUBIC, nullptr, nullptr, nullptr));
  std::unique_ptr<AVFrame, AVFrameConvertedDeleter> rgb_frame(av_frame_alloc());
  av_frame_copy_props(rgb_frame.get(), frame);
  rgb_frame->format = format;
  rgb_frame->width = size.width;
  rgb_frame->height = size.height;
  Check(av_image_alloc(rgb_frame->data, rgb_frame->linesize, size.width,
                       size.height, format, 32),
        "av_image_alloc");
  Check(sws_scale(sws_context.get(), frame->data, frame->linesize, 0,
                  frame->height, rgb_frame->data, rgb_frame->linesize),
        "sws_scale");
  return rgb_frame;
}

std::string EncodeFrame(AVFrame* input_frame, ThumbnailOptions options) {
  auto size =
      GetThumbnailSize({input_frame->width, input_frame->height}, options.size);
  auto codec = avcodec_find_encoder(
      options.codec == ThumbnailOptions::Codec::JPEG ? AV_CODEC_ID_MJPEG
                                                     : AV_CODEC_ID_PNG);
  if (!codec) {
    throw std::logic_error("codec not found");
  }
  int loss_ptr;
  auto frame =
      ConvertFrame(input_frame, size,
                   avcodec_find_best_pix_fmt_of_list(
                       codec->pix_fmts, AVPixelFormat(input_frame->format),
                       false, &loss_ptr));
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> context(
      avcodec_alloc_context3(codec));
  context->time_base = {1, 24};
  context->pix_fmt = AVPixelFormat(frame->format);
  context->width = frame->width;
  context->height = frame->height;
  Check(avcodec_open2(context.get(), codec, nullptr), "avcodec_open2");
  auto packet = CreatePacket();
  bool frame_sent = false, flush_sent = false;
  std::string result;
  while (true) {
    if (!frame_sent) {
      Check(avcodec_send_frame(context.get(), frame.get()),
            "avcodec_send_frame");
      frame_sent = true;
    } else if (!flush_sent) {
      Check(avcodec_send_frame(context.get(), nullptr), "avcodec_send_frame");
      flush_sent = true;
    }
    auto err = avcodec_receive_packet(context.get(), packet.get());
    if (err != 0) {
      if (err == AVERROR_EOF) {
        break;
      } else {
        Check(err, "avcodec_receive_packet");
      }
    } else {
      result +=
          std::string(reinterpret_cast<char*>(packet->data), packet->size);
    }
  }
  return result;
}

auto CreateSourceFilter(AVFormatContext* format_context, int stream,
                        AVCodecContext* codec_context, AVFilterGraph* graph) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter buffer unavailable");
  }
  AVDictionary* d = nullptr;
  av_dict_set_int(&d, "width", codec_context->width, 0);
  av_dict_set_int(&d, "height", codec_context->height, 0);
  av_dict_set_int(&d, "pix_fmt", codec_context->pix_fmt, 0);
  av_dict_set(
      &d, "time_base",
      (std::to_string(format_context->streams[stream]->time_base.num) + "/" +
       std::to_string(format_context->streams[stream]->time_base.den))
          .c_str(),
      0);
  auto err = avfilter_init_dict(filter.get(), &d);
  av_dict_free(&d);
  Check(err, "avfilter_init_dict source");
  return filter;
}

auto CreateSinkFilter(AVFilterGraph* graph) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter buffersink unavailable");
  }
  Check(avfilter_init_dict(filter.get(), nullptr), "avfilter_init_dict");
  return filter;
}

auto CreateThumbnailFilter(AVFilterGraph* graph) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("thumbnail"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter thumbnail unavailable");
  }
  Check(avfilter_init_dict(filter.get(), nullptr), "avfilter_init_dict");
  return filter;
}

auto CreateScaleFilter(AVFilterGraph* graph, ImageSize size) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("scale"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter thumbnail unavailable");
  }
  AVDictionary* d = nullptr;
  av_dict_set_int(&d, "width", size.width, 0);
  av_dict_set_int(&d, "height", size.height, 0);
  auto err = avfilter_init_dict(filter.get(), &d);
  av_dict_free(&d);
  Check(err, "avfilter_init_dict");
  return filter;
}

auto GetThumbnailFrame(AVIOContext* io_context, ThumbnailOptions options) {
  auto context = CreateFormatContext(io_context);
  auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
  Check(stream, "av_find_best_stream");
  if (context->duration > 0) {
    Check(av_seek_frame(context.get(), -1, context->duration / 10, 0),
          "av_seek_frame");
  }
  auto codec_context = CreateCodecContext(context.get(), stream);
  auto size = GetThumbnailSize({codec_context->width, codec_context->height},
                               options.size);
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> filter_graph(
      avfilter_graph_alloc());
  auto source_filter = CreateSourceFilter(
      context.get(), stream, codec_context.get(), filter_graph.get());
  auto sink_filter = CreateSinkFilter(filter_graph.get());
  auto thumbnail_filter = CreateThumbnailFilter(filter_graph.get());
  auto scale_filter = CreateScaleFilter(filter_graph.get(), size);
  Check(avfilter_link(source_filter.get(), 0, scale_filter.get(), 0),
        "avfilter_link");
  Check(avfilter_link(scale_filter.get(), 0, thumbnail_filter.get(), 0),
        "avfilter_link");
  Check(avfilter_link(thumbnail_filter.get(), 0, sink_filter.get(), 0),
        "avfilter_link");
  Check(avfilter_graph_config(filter_graph.get(), nullptr),
        "avfilter_graph_config");
  std::unique_ptr<AVFrame, AVFrameDeleter> frame;
  while (auto current =
             DecodeFrame(context.get(), codec_context.get(), stream)) {
    frame = std::move(current);
    Check(av_buffersrc_write_frame(source_filter.get(), frame.get()),
          "av_buffersrc_write_frame");
    std::unique_ptr<AVFrame, AVFrameDeleter> received_frame(av_frame_alloc());
    auto err = av_buffersink_get_frame(sink_filter.get(), received_frame.get());
    if (err == 0) {
      frame = std::move(received_frame);
      break;
    } else if (err != AVERROR(EAGAIN)) {
      Check(err, "av_buffersink_get_frame");
    }
  }
  if (!frame) {
    throw std::logic_error("couldn't get any frame");
  }
  return frame;
}

}  // namespace

std::string GenerateThumbnail(AVIOContext* io_context,
                              ThumbnailOptions options) {
  return EncodeFrame(GetThumbnailFrame(io_context, options).get(), options);
}

}  // namespace coro::cloudstorage::util