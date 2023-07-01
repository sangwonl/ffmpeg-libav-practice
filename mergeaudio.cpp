#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

#define inputPixelFormat "uyvy422"
#define inputFps 30
#define ouptutChannels 2
#define outputSampleRate 48000
#define outputFilename "output.mp4"

typedef struct MediaParams {
    AVCodecID codecId;
    AVMediaType mediaType;
    int width;
    int height;
    int channels;
    int frameRate;
    int sampleRate;
} MediaParams;

typedef struct MediaContext {
  char filename[64];
  AVFormatContext *formatCtx;

  int videoIndex;
  AVCodec* videoCodec;
  AVStream* videoStream;
  AVCodecContext* videoCodecCtx;
  AVFilterContext *videoBufferFilterCtx;
  SwsContext* swsCtx;

  int audioIndex;
  AVCodec* audioCodec;
  AVStream* audioStream;
  AVCodecContext* audioCodecCtx;
  AVFilterContext *audioBufferFilterCtx;
} MediaContext;

bool shouldStop = false;
bool allDone = false;

void signalHandler(int signum) {
    if (signum == SIGINT) {
        std::cout << "signaled\n";
        shouldStop = true;
    }
}

MediaContext* openInputMediaCtx(int screenIdx, int audioIdx, AVPixelFormat normalizedPixFmt) {
    MediaContext* mediaCtx = (MediaContext*) calloc(1, sizeof(MediaContext));

    AVDictionary* options = nullptr;
    av_dict_set(&options, "framerate", std::to_string(inputFps).c_str(), 0);
    av_dict_set(&options, "pixel_format", inputPixelFormat, 0);
    av_dict_set(&options, "capture_cursor", "1", 0);

    // Open screen capture input
    const AVInputFormat* inputFormat = nullptr;
// #ifdef __APPLE__
    inputFormat = av_find_input_format("avfoundation");
// #elif defined(__linux__)
//     inputFormat = av_find_input_format("x11grab");
// #endif

    snprintf(mediaCtx->filename, sizeof(mediaCtx->filename), "%d:%d", screenIdx, audioIdx);
    if (avformat_open_input(&mediaCtx->formatCtx, mediaCtx->filename, inputFormat, &options) < 0) {
        return nullptr;
    }

    if (avformat_find_stream_info(mediaCtx->formatCtx, nullptr) < 0) {
        return nullptr;
    }

    int videoStreamIdx = av_find_best_stream(mediaCtx->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx >= 0) {
        mediaCtx->videoStream = mediaCtx->formatCtx->streams[videoStreamIdx];
        mediaCtx->videoCodec = const_cast<AVCodec*>(avcodec_find_decoder(mediaCtx->videoStream->codecpar->codec_id));
        mediaCtx->videoCodecCtx = avcodec_alloc_context3(mediaCtx->videoCodec);
        avcodec_parameters_to_context(mediaCtx->videoCodecCtx, mediaCtx->videoStream->codecpar);
        avcodec_open2(mediaCtx->videoCodecCtx, mediaCtx->videoCodec, nullptr);
    }

    int audioStreamIdx = av_find_best_stream(mediaCtx->formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx >= 0) {
        mediaCtx->audioStream = mediaCtx->formatCtx->streams[audioStreamIdx];
        mediaCtx->audioCodec = const_cast<AVCodec*>(avcodec_find_decoder(mediaCtx->audioStream->codecpar->codec_id));
        mediaCtx->audioCodecCtx = avcodec_alloc_context3(mediaCtx->audioCodec);
        avcodec_parameters_to_context(mediaCtx->audioCodecCtx, mediaCtx->audioStream->codecpar);
        avcodec_open2(mediaCtx->audioCodecCtx, mediaCtx->audioCodec, nullptr);
    }

    mediaCtx->swsCtx = sws_getContext(
        mediaCtx->videoCodecCtx->width,
        mediaCtx->videoCodecCtx->height,
        mediaCtx->videoCodecCtx->pix_fmt,
        mediaCtx->videoCodecCtx->width,
        mediaCtx->videoCodecCtx->height,
        normalizedPixFmt,
        SWS_BICUBIC,
        NULL,
        NULL,
        NULL
    );

    av_dict_free(&options);

    return mediaCtx;
}

void prepareVideoCodec(MediaContext* mediaCtx, MediaParams* params) {
    mediaCtx->videoCodec = const_cast<AVCodec*>(avcodec_find_encoder(mediaCtx->formatCtx->oformat->video_codec));
    if (mediaCtx->videoCodec == nullptr) {
        return;
    }

    mediaCtx->videoStream = avformat_new_stream(mediaCtx->formatCtx, mediaCtx->videoCodec);
    if (mediaCtx->videoStream == nullptr) {
        return;
    }

    mediaCtx->videoCodecCtx = avcodec_alloc_context3(mediaCtx->videoCodec);
    if (mediaCtx->videoCodecCtx == nullptr) {
        return;
    }

    mediaCtx->videoCodecCtx->width = params->width;
    mediaCtx->videoCodecCtx->height = params->height;
    mediaCtx->videoCodecCtx->sample_aspect_ratio = av_make_q(0, 1);
    mediaCtx->videoCodecCtx->time_base = av_make_q(1, 1000);
    mediaCtx->videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avcodec_open2(mediaCtx->videoCodecCtx, mediaCtx->videoCodec, nullptr) < 0) {
        return ;
    }

    avcodec_parameters_from_context(mediaCtx->videoStream->codecpar, mediaCtx->videoCodecCtx);
}

void prepareAudioCodec(MediaContext* mediaCtx, MediaParams* params) {
    mediaCtx->audioCodec = const_cast<AVCodec*>(avcodec_find_encoder(mediaCtx->formatCtx->oformat->audio_codec));
    if (mediaCtx->audioCodec == nullptr) {
        return;
    }

    mediaCtx->audioStream = avformat_new_stream(mediaCtx->formatCtx, mediaCtx->audioCodec);
    if (mediaCtx->audioStream == nullptr) {
        return;
    }

    mediaCtx->audioCodecCtx = avcodec_alloc_context3(mediaCtx->audioCodec);
    if (mediaCtx->audioCodecCtx == nullptr) {
        return;
    }

    AVChannelLayout channelLayout;
    av_channel_layout_default(&channelLayout, params->channels);

    mediaCtx->audioCodecCtx->ch_layout = channelLayout;
    mediaCtx->audioCodecCtx->sample_rate = params->sampleRate;
    mediaCtx->audioCodecCtx->sample_fmt = mediaCtx->audioCodec->sample_fmts[0];
    mediaCtx->audioCodecCtx->time_base = av_make_q(1, params->sampleRate);
    mediaCtx->audioStream->time_base = mediaCtx->audioCodecCtx->time_base;

    if (avcodec_open2(mediaCtx->audioCodecCtx, mediaCtx->audioCodec, nullptr) < 0) {
        return;
    }

    avcodec_parameters_from_context(mediaCtx->audioStream->codecpar, mediaCtx->audioCodecCtx);
}

MediaContext* openOutputMediaCtx(const char* filename, MediaParams* videoParams, MediaParams* audioParams) {
    MediaContext* mediaCtx = (MediaContext*) calloc(1, sizeof(MediaContext));

    if (avformat_alloc_output_context2(&mediaCtx->formatCtx, nullptr, nullptr, filename) < 0) {
        return nullptr;
    }

    if (videoParams != nullptr) {
        prepareVideoCodec(mediaCtx, videoParams);
    }

    if (videoParams != nullptr) {
        prepareAudioCodec(mediaCtx, audioParams);
    }

    // Open the output file
    if (avio_open(&mediaCtx->formatCtx->pb, filename, AVIO_FLAG_WRITE) != 0) {
        return nullptr;
    }

    return mediaCtx;
}

AVFilterGraph* createFilterGraphForCropAndMerge(MediaContext* inputCtx1, MediaContext* inputCtx2, MediaContext* outputCtx, int cropX, int cropY, int cropWidth, int cropHeight) {
    AVFilterGraph *filterGraph = avfilter_graph_alloc();

    AVFilterContext *crop1Ctx;
    AVFilterContext *crop2Ctx;
    AVFilterContext *padCtx;
    AVFilterContext *overlayCtx;

    const AVFilter *bufferSrcFilter = avfilter_get_by_name("buffer");
    const AVFilter *cropFilter = avfilter_get_by_name("crop");
    const AVFilter *padFilter = avfilter_get_by_name("pad");
    const AVFilter *overlayFilter = avfilter_get_by_name("overlay");
    const AVFilter *bufferSinkFilter = avfilter_get_by_name("buffersink");

    char filterArgs[512];
    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        inputCtx1->videoCodecCtx->width,
        inputCtx1->videoCodecCtx->height,
        outputCtx->videoCodecCtx->pix_fmt,
        outputCtx->videoCodecCtx->time_base.num,
        outputCtx->videoCodecCtx->time_base.den,
        inputCtx1->videoCodecCtx->sample_aspect_ratio.num,
        inputCtx1->videoCodecCtx->sample_aspect_ratio.den);

    if (avfilter_graph_create_filter(&inputCtx1->videoBufferFilterCtx, bufferSrcFilter, "in1", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        inputCtx2->videoCodecCtx->width,
        inputCtx2->videoCodecCtx->height,
        outputCtx->videoCodecCtx->pix_fmt,
        outputCtx->videoCodecCtx->time_base.num,
        outputCtx->videoCodecCtx->time_base.den,
        inputCtx2->videoCodecCtx->sample_aspect_ratio.num,
        inputCtx2->videoCodecCtx->sample_aspect_ratio.den);

    if (avfilter_graph_create_filter(&inputCtx2->videoBufferFilterCtx, bufferSrcFilter, "in2", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);

    if (avfilter_graph_create_filter(&crop1Ctx, cropFilter, "crop1", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);

    if (avfilter_graph_create_filter(&crop2Ctx, cropFilter, "crop2", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d", // w:h:x:y
        cropWidth * 2,
        cropHeight,
        0,
        0);

    if (avfilter_graph_create_filter(&padCtx, padFilter, "pad", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d",
        cropWidth,
        0);

    if (avfilter_graph_create_filter(&overlayCtx, overlayFilter, "overlay", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_graph_create_filter(&outputCtx->videoBufferFilterCtx, bufferSinkFilter, "out", nullptr, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_link(inputCtx1->videoBufferFilterCtx, 0, crop1Ctx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(crop1Ctx, 0, padCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(padCtx, 0, overlayCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(inputCtx2->videoBufferFilterCtx, 0, crop2Ctx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(crop2Ctx, 0, overlayCtx, 1) < 0) {
        return nullptr;
    }

    if (avfilter_link(overlayCtx, 0, outputCtx->videoBufferFilterCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_graph_config(filterGraph, nullptr) < 0) {
        return nullptr;
    }

    return filterGraph;
}

int main() {
    std::signal(SIGINT, signalHandler);

    // Initialize FFmpeg
    avdevice_register_all();

    const int cropX = 100;
    const int cropY = 0;
    const int cropWidth = 500;
    const int cropHeight = 800;

    MediaParams videoParams = { .width = cropWidth * 2, .height = cropHeight };
    MediaParams audioParams = { .channels = ouptutChannels, .sampleRate = outputSampleRate };
    MediaContext* outputCtx = openOutputMediaCtx(outputFilename, &videoParams, &audioParams);
    MediaContext* inputCtx1 = openInputMediaCtx(2, 0, outputCtx->videoCodecCtx->pix_fmt);
    MediaContext* inputCtx2 = openInputMediaCtx(3, 2, outputCtx->videoCodecCtx->pix_fmt);

    createFilterGraphForCropAndMerge(inputCtx1, inputCtx2, outputCtx, cropX, cropY, cropWidth, cropHeight);

    // Read and encode frames
    AVPacket *input1Packet = av_packet_alloc();
    AVPacket *input2Packet = av_packet_alloc();
    AVPacket *outputPacket = av_packet_alloc();

    AVFrame *input1Frame = av_frame_alloc();
    AVFrame *input2Frame = av_frame_alloc();
    AVFrame *yuv1Frame = av_frame_alloc();
    AVFrame *yuv2Frame = av_frame_alloc();
    AVFrame *filteredFrame = av_frame_alloc();

    int64_t numFrames = -1;

    // Write the header to the output file
    if (avformat_write_header(outputCtx->formatCtx, nullptr) < 0) {
        return -1;
    }

    while (!allDone) {
        if (av_read_frame(inputCtx1->formatCtx, input1Packet) == 0) {
            if (input1Packet->stream_index == inputCtx1->videoIndex) {
                avcodec_send_packet(inputCtx1->videoCodecCtx, input1Packet);
            }
        }

        if (av_read_frame(inputCtx2->formatCtx, input2Packet) == 0) {
            if (input2Packet->stream_index == inputCtx2->videoIndex) {
                avcodec_send_packet(inputCtx2->videoCodecCtx, input2Packet);
            }
        }

        if (avcodec_receive_frame(inputCtx1->videoCodecCtx, input1Frame) == 0) {
            yuv1Frame->format = outputCtx->videoCodecCtx->pix_fmt;
            yuv1Frame->width = inputCtx1->videoCodecCtx->width;
            yuv1Frame->height = inputCtx1->videoCodecCtx->height;
            av_frame_get_buffer(yuv1Frame, 0);

            sws_scale(inputCtx1->swsCtx, input1Frame->data, input1Frame->linesize, 0, input1Frame->height, yuv1Frame->data, yuv1Frame->linesize);

            av_buffersrc_add_frame(inputCtx1->videoBufferFilterCtx, yuv1Frame);
        }

        if (avcodec_receive_frame(inputCtx2->videoCodecCtx, input2Frame) == 0) {
            yuv2Frame->format = outputCtx->videoCodecCtx->pix_fmt;
            yuv2Frame->width = inputCtx2->videoCodecCtx->width;
            yuv2Frame->height = inputCtx2->videoCodecCtx->height;
            av_frame_get_buffer(yuv2Frame, 0);

            sws_scale(inputCtx2->swsCtx, input2Frame->data, input2Frame->linesize, 0, input2Frame->height, yuv2Frame->data, yuv2Frame->linesize);

            av_buffersrc_add_frame(inputCtx2->videoBufferFilterCtx, yuv2Frame);
        }

        if (av_buffersink_get_frame(outputCtx->videoBufferFilterCtx, filteredFrame) == 0) {
            numFrames++;

            // Rescale timestamps
            filteredFrame->pts = av_rescale_rnd(numFrames, outputCtx->videoCodecCtx->time_base.den, inputFps, AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

            if (!shouldStop) {
                avcodec_send_frame(outputCtx->videoCodecCtx, filteredFrame);
            }
        }

        int ret = avcodec_receive_packet(outputCtx->videoCodecCtx, outputPacket);
        if (ret == 0) {
            outputPacket->stream_index = outputCtx->videoStream->index;
            av_packet_rescale_ts(outputPacket, outputCtx->videoCodecCtx->time_base, outputCtx->videoStream->time_base);

            // Write the packet to the output file
            av_interleaved_write_frame(outputCtx->formatCtx, outputPacket);
        } else if (ret == AVERROR(EAGAIN)) {
            allDone = shouldStop;
        }

        av_frame_unref(filteredFrame);
        av_frame_unref(yuv1Frame);
        av_frame_unref(yuv2Frame);
        av_frame_unref(input1Frame);
        av_frame_unref(input2Frame);
        av_packet_unref(input1Packet);
        av_packet_unref(input2Packet);
        av_packet_unref(outputPacket);
    }

    // Write the trailer to the output file
    av_write_trailer(outputCtx->formatCtx);

    // Cleanup
    avformat_close_input(&inputCtx1->formatCtx);
    avformat_close_input(&inputCtx2->formatCtx);
    avformat_free_context(outputCtx->formatCtx);
    avformat_network_deinit();

    return 0;
}