#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/audio_fifo.h>
}

#define inputPixelFormat "uyvy422"
#define inputFps 30
#define ouptutChannels 2
#define outputSampleRate 48000
#define outputFilename "output.mp4"

static const AVRational videoEncoderTimeBase = av_make_q(1, 1000);
static const AVRational videoContainerTimeBase = av_make_q(1, 16000);
static const AVRational audioEncoderTimeBase = av_make_q(1, outputSampleRate);
static const AVRational audioContainerTimeBase = av_make_q(1, outputSampleRate);

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
  SwrContext* swrCtx;
  AVAudioFifo* audioFifo;
} MediaContext;

bool shouldStop = false;
bool allDone = false;

void signalHandler(int signum) {
    if (signum == SIGINT) {
        std::cout << "signaled\n";
        shouldStop = true;
    }
}

MediaContext* openInputMediaCtx(int screenIdx, int audioIdx, MediaContext* outMediaCtx) {
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
        mediaCtx->videoIndex = videoStreamIdx;
        mediaCtx->videoStream = mediaCtx->formatCtx->streams[videoStreamIdx];
        mediaCtx->videoCodec = const_cast<AVCodec*>(avcodec_find_decoder(mediaCtx->videoStream->codecpar->codec_id));
        mediaCtx->videoCodecCtx = avcodec_alloc_context3(mediaCtx->videoCodec);
        mediaCtx->videoCodecCtx->time_base = mediaCtx->videoStream->time_base;

        avcodec_parameters_to_context(mediaCtx->videoCodecCtx, mediaCtx->videoStream->codecpar);
        avcodec_open2(mediaCtx->videoCodecCtx, mediaCtx->videoCodec, nullptr);

        mediaCtx->swsCtx = sws_getContext(
            mediaCtx->videoCodecCtx->width,
            mediaCtx->videoCodecCtx->height,
            mediaCtx->videoCodecCtx->pix_fmt,
            mediaCtx->videoCodecCtx->width,
            mediaCtx->videoCodecCtx->height,
            outMediaCtx->videoCodecCtx->pix_fmt,
            SWS_BICUBIC,
            NULL,
            NULL,
            NULL
        );
    }

    int audioStreamIdx = av_find_best_stream(mediaCtx->formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx >= 0) {
        mediaCtx->audioIndex = audioStreamIdx;
        mediaCtx->audioStream = mediaCtx->formatCtx->streams[audioStreamIdx];
        mediaCtx->audioCodec = const_cast<AVCodec*>(avcodec_find_decoder(mediaCtx->audioStream->codecpar->codec_id));
        mediaCtx->audioCodecCtx = avcodec_alloc_context3(mediaCtx->audioCodec);
        mediaCtx->audioCodecCtx->time_base = mediaCtx->audioStream->time_base;

        avcodec_parameters_to_context(mediaCtx->audioCodecCtx, mediaCtx->audioStream->codecpar);
        avcodec_open2(mediaCtx->audioCodecCtx, mediaCtx->audioCodec, nullptr);

        swr_alloc_set_opts2(&mediaCtx->swrCtx,
            &outMediaCtx->audioCodecCtx->ch_layout,
            outMediaCtx->audioCodecCtx->sample_fmt,
            outMediaCtx->audioCodecCtx->sample_rate,
            &mediaCtx->audioCodecCtx->ch_layout,
            mediaCtx->audioCodecCtx->sample_fmt,
            mediaCtx->audioCodecCtx->sample_rate,
            0,
            nullptr);

        swr_init(mediaCtx->swrCtx);

        mediaCtx->audioFifo = av_audio_fifo_alloc(
            outMediaCtx->audioCodecCtx->sample_fmt,
            outMediaCtx->audioCodecCtx->ch_layout.nb_channels,
            1
        );
    }

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

    mediaCtx->videoIndex = 0;
    mediaCtx->videoCodecCtx->width = params->width;
    mediaCtx->videoCodecCtx->height = params->height;
    mediaCtx->videoCodecCtx->sample_aspect_ratio = av_make_q(0, 1);
    mediaCtx->videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    mediaCtx->videoCodecCtx->time_base = videoEncoderTimeBase;
    mediaCtx->videoStream->time_base = videoContainerTimeBase;

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

    mediaCtx->audioIndex = 1;
    mediaCtx->audioCodecCtx->ch_layout = channelLayout;
    mediaCtx->audioCodecCtx->sample_rate = params->sampleRate;
    mediaCtx->audioCodecCtx->sample_fmt = mediaCtx->audioCodec->sample_fmts[0];
    mediaCtx->audioCodecCtx->time_base = audioEncoderTimeBase;
    mediaCtx->audioStream->time_base = audioContainerTimeBase;

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

AVFilterGraph* createFilterGraphForVideo(MediaContext* input1Ctx, MediaContext* input2Ctx, MediaContext* outputCtx, int cropX, int cropY, int cropWidth, int cropHeight) {
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
        input1Ctx->videoCodecCtx->width,
        input1Ctx->videoCodecCtx->height,
        outputCtx->videoCodecCtx->pix_fmt,
        input1Ctx->videoCodecCtx->time_base.num,
        input1Ctx->videoCodecCtx->time_base.den,
        input1Ctx->videoCodecCtx->sample_aspect_ratio.num,
        input1Ctx->videoCodecCtx->sample_aspect_ratio.den);
    if (avfilter_graph_create_filter(&input1Ctx->videoBufferFilterCtx, bufferSrcFilter, "v-in1", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        input2Ctx->videoCodecCtx->width,
        input2Ctx->videoCodecCtx->height,
        outputCtx->videoCodecCtx->pix_fmt,
        input2Ctx->videoCodecCtx->time_base.num,
        input2Ctx->videoCodecCtx->time_base.den,
        input2Ctx->videoCodecCtx->sample_aspect_ratio.num,
        input2Ctx->videoCodecCtx->sample_aspect_ratio.den);
    if (avfilter_graph_create_filter(&input2Ctx->videoBufferFilterCtx, bufferSrcFilter, "v-in2", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);
    if (avfilter_graph_create_filter(&crop1Ctx, cropFilter, "v-crop1", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);
    if (avfilter_graph_create_filter(&crop2Ctx, cropFilter, "v-crop2", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d", // w:h:x:y
        cropWidth * 2,
        cropHeight,
        0,
        0);
    if (avfilter_graph_create_filter(&padCtx, padFilter, "v-pad", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d",
        cropWidth,
        0);
    if (avfilter_graph_create_filter(&overlayCtx, overlayFilter, "v-overlay", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_graph_create_filter(&outputCtx->videoBufferFilterCtx, bufferSinkFilter, "v-out", nullptr, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_link(input1Ctx->videoBufferFilterCtx, 0, crop1Ctx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(crop1Ctx, 0, padCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(padCtx, 0, overlayCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(input2Ctx->videoBufferFilterCtx, 0, crop2Ctx, 0) < 0) {
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

AVFilterGraph* createFilterGraphForAudio(MediaContext* input1Ctx, MediaContext* input2Ctx, MediaContext* outputCtx) {
    AVFilterGraph *filterGraph = avfilter_graph_alloc();

    AVFilterContext *mergeCtx;
    AVFilterContext *panCtx;

    const AVFilter *bufferSrcFilter = avfilter_get_by_name("abuffer");
    const AVFilter *mergeFilter = avfilter_get_by_name("amerge");
    const AVFilter *panFilter = avfilter_get_by_name("pan");
    const AVFilter *bufferSinkFilter = avfilter_get_by_name("abuffersink");

    char filterArgs[512];
    snprintf(filterArgs, sizeof(filterArgs),
        "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
        outputCtx->audioCodecCtx->time_base.num,
        outputCtx->audioCodecCtx->time_base.den,
        input1Ctx->audioCodecCtx->sample_rate,
        av_get_sample_fmt_name(input1Ctx->audioCodecCtx->sample_fmt),
        input1Ctx->audioCodecCtx->channel_layout);
    if (avfilter_graph_create_filter(&input1Ctx->audioBufferFilterCtx, bufferSrcFilter, "a-in1", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
        outputCtx->audioCodecCtx->time_base.num,
        outputCtx->audioCodecCtx->time_base.den,
        input2Ctx->audioCodecCtx->sample_rate,
        av_get_sample_fmt_name(input2Ctx->audioCodecCtx->sample_fmt),
        input2Ctx->audioCodecCtx->channel_layout);
    if (avfilter_graph_create_filter(&input2Ctx->audioBufferFilterCtx, bufferSrcFilter, "a-in2", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs), "inputs=%d", 2);
    if (avfilter_graph_create_filter(&mergeCtx, mergeFilter, "a-amerge", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    snprintf(filterArgs, sizeof(filterArgs), "stereo|FL=c0+c2|FR=c1+c2");
    if (avfilter_graph_create_filter(&panCtx, panFilter, "a-pan", filterArgs, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_graph_create_filter(&outputCtx->audioBufferFilterCtx, bufferSinkFilter, "a-out", nullptr, nullptr, filterGraph) < 0) {
        return nullptr;
    }

    if (avfilter_link(input1Ctx->audioBufferFilterCtx, 0, mergeCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(input2Ctx->audioBufferFilterCtx, 0, mergeCtx, 1) < 0) {
        return nullptr;
    }

    if (avfilter_link(mergeCtx, 0, panCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_link(panCtx, 0, outputCtx->audioBufferFilterCtx, 0) < 0) {
        return nullptr;
    }

    if (avfilter_graph_config(filterGraph, nullptr) < 0) {
        return nullptr;
    }

    return filterGraph;
}

void convert_video_frame(AVFrame* inputFrame, AVFrame* yuvFrame, MediaContext* inputCtx, MediaContext* outputCtx) {
    yuvFrame->format = outputCtx->videoCodecCtx->pix_fmt;
    yuvFrame->width = inputCtx->videoCodecCtx->width;
    yuvFrame->height = inputCtx->videoCodecCtx->height;
    av_frame_get_buffer(yuvFrame, 0);
    sws_scale(inputCtx->swsCtx, inputFrame->data, inputFrame->linesize, 0, inputFrame->height, yuvFrame->data, yuvFrame->linesize);
}

int convert_audio_frame(AVFrame* inputFrame, AVFrame* resampledFrame, MediaContext* inputCtx, MediaContext* outputCtx) {
    const AVSampleFormat outFormat = outputCtx->audioCodecCtx->sample_fmt;
    const AVChannelLayout* outChLayout = &outputCtx->audioCodecCtx->ch_layout;
    const int outFrameSize = outputCtx->audioCodecCtx->frame_size;
    const int outSampleRate = outputCtx->audioCodecCtx->sample_rate;
    const int inputFrameSize = inputFrame->nb_samples;

    int curFifoSize = av_audio_fifo_size(inputCtx->audioFifo);
    if (curFifoSize < outFrameSize) {
        uint8_t** convertedSamples = (uint8_t**)calloc(outChLayout->nb_channels, sizeof(*convertedSamples));
        int ret = av_samples_alloc(convertedSamples, nullptr, outChLayout->nb_channels, inputFrameSize, outFormat, 0);
        swr_convert(inputCtx->swrCtx, convertedSamples, inputFrameSize, (const uint8_t**)inputFrame->extended_data, inputFrameSize);

        av_audio_fifo_realloc(inputCtx->audioFifo, curFifoSize + inputFrameSize);
        av_audio_fifo_write(inputCtx->audioFifo, (void**)convertedSamples, inputFrameSize);
    }

    curFifoSize = av_audio_fifo_size(inputCtx->audioFifo);
    if (curFifoSize >= outFrameSize) {
        const int readFrameSize = FFMIN(av_audio_fifo_size(inputCtx->audioFifo), outputCtx->audioCodecCtx->frame_size);
        resampledFrame->nb_samples = readFrameSize;
        resampledFrame->format = outFormat;
        resampledFrame->sample_rate = outSampleRate;
        av_channel_layout_copy(&resampledFrame->ch_layout, outChLayout);
        av_frame_get_buffer(resampledFrame, 0);

        av_audio_fifo_read(inputCtx->audioFifo, (void**)resampledFrame->data, readFrameSize);

        return readFrameSize;
    }

    return 0;
}

int main() {
    std::signal(SIGINT, signalHandler);

    avdevice_register_all();

    const int cropX = 100;
    const int cropY = 0;
    const int cropWidth = 500;
    const int cropHeight = 800;

    MediaParams videoParams = { .width = cropWidth * 2, .height = cropHeight };
    MediaParams audioParams = { .channels = ouptutChannels, .sampleRate = outputSampleRate };
    MediaContext* outputCtx = openOutputMediaCtx(outputFilename, &videoParams, &audioParams);
    MediaContext* input1Ctx = openInputMediaCtx(0, 0, outputCtx);
    MediaContext* input2Ctx = openInputMediaCtx(2, 2, outputCtx);

    swr_alloc_set_opts2(&outputCtx->swrCtx,
        &outputCtx->audioCodecCtx->ch_layout,
        outputCtx->audioCodecCtx->sample_fmt,
        outputCtx->audioCodecCtx->sample_rate,
        &input1Ctx->audioCodecCtx->ch_layout,
        input1Ctx->audioCodecCtx->sample_fmt,
        input1Ctx->audioCodecCtx->sample_rate,
        0,
        nullptr);

    swr_init(outputCtx->swrCtx);

    outputCtx->audioFifo = av_audio_fifo_alloc(
        outputCtx->audioCodecCtx->sample_fmt,
        outputCtx->audioCodecCtx->ch_layout.nb_channels,
        1
    );

    createFilterGraphForVideo(input1Ctx, input2Ctx, outputCtx, cropX, cropY, cropWidth, cropHeight);
    createFilterGraphForAudio(input1Ctx, input2Ctx, outputCtx);

    // Read and encode frames
    AVPacket *input1Packet = av_packet_alloc();
    AVPacket *input2Packet = av_packet_alloc();
    AVPacket *outputVidPacket = av_packet_alloc();
    AVPacket *outputAudPacket = av_packet_alloc();

    AVFrame *input1VidFrame = av_frame_alloc();
    AVFrame *input2VidFrame = av_frame_alloc();
    AVFrame *input1YuvFrame = av_frame_alloc();
    AVFrame *input2YuvFrame = av_frame_alloc();
    AVFrame *filteredVidFrame = av_frame_alloc();

    AVFrame *input1AudFrame = av_frame_alloc();
    AVFrame *input2AudFrame = av_frame_alloc();
    AVFrame *filteredAudFrame = av_frame_alloc();
    AVFrame *filteredResampledFrame = av_frame_alloc();

    int64_t numVidFrames = 0;
    int64_t numAudSamples = 0;

    // Write the header to the output file
    if (avformat_write_header(outputCtx->formatCtx, nullptr) < 0) {
        return -1;
    }

    while (!allDone) {
        if (av_read_frame(input1Ctx->formatCtx, input1Packet) == 0) {
            if (input1Packet->stream_index == input1Ctx->videoIndex) {
                avcodec_send_packet(input1Ctx->videoCodecCtx, input1Packet);
            } else if (input1Packet->stream_index == input1Ctx->audioIndex) {
                avcodec_send_packet(input1Ctx->audioCodecCtx, input1Packet);
            }
        }

        if (av_read_frame(input2Ctx->formatCtx, input2Packet) == 0) {
            if (input2Packet->stream_index == input2Ctx->videoIndex) {
                avcodec_send_packet(input2Ctx->videoCodecCtx, input2Packet);
            } else if (input2Packet->stream_index == input2Ctx->audioIndex) {
                avcodec_send_packet(input2Ctx->audioCodecCtx, input2Packet);
            }
        }

        if (avcodec_receive_frame(input1Ctx->videoCodecCtx, input1VidFrame) == 0) {
            convert_video_frame(input1VidFrame, input1YuvFrame, input1Ctx, outputCtx);
            av_buffersrc_add_frame(input1Ctx->videoBufferFilterCtx, input1YuvFrame);
        }

        if (avcodec_receive_frame(input2Ctx->videoCodecCtx, input2VidFrame) == 0) {
            convert_video_frame(input2VidFrame, input2YuvFrame, input2Ctx, outputCtx);
            av_buffersrc_add_frame(input2Ctx->videoBufferFilterCtx, input2YuvFrame);
        }

        if (avcodec_receive_frame(input1Ctx->audioCodecCtx, input1AudFrame) == 0) {
            av_buffersrc_add_frame(input1Ctx->audioBufferFilterCtx, input1AudFrame);
        }

        if (avcodec_receive_frame(input2Ctx->audioCodecCtx, input2AudFrame) == 0) {
            av_buffersrc_add_frame(input2Ctx->audioBufferFilterCtx, input2AudFrame);
        }

        if (av_buffersink_get_frame(outputCtx->videoBufferFilterCtx, filteredVidFrame) == 0) {
            filteredVidFrame->pts = av_rescale_q_rnd(numVidFrames++,
                (AVRational){1, inputFps},
                outputCtx->videoCodecCtx->time_base,
                AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

            if (!shouldStop) {
                avcodec_send_frame(outputCtx->videoCodecCtx, filteredVidFrame);
            }
        }

        if (av_buffersink_get_frame(outputCtx->audioBufferFilterCtx, filteredAudFrame) == 0) {
            int convertedSize = convert_audio_frame(filteredAudFrame, filteredResampledFrame, input1Ctx, outputCtx);
            if (convertedSize > 0) {
                filteredResampledFrame->pts = av_rescale_q_rnd(numAudSamples,
                (AVRational){1, outputCtx->audioCodecCtx->sample_rate},
                outputCtx->audioCodecCtx->time_base,
                AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                numAudSamples += convertedSize;

                if (!shouldStop) {
                    avcodec_send_frame(outputCtx->audioCodecCtx, filteredResampledFrame);
                }
            }
        }

        int ret = avcodec_receive_packet(outputCtx->videoCodecCtx, outputVidPacket);
        if (ret == 0) {
            outputVidPacket->stream_index = outputCtx->videoIndex;
            av_packet_rescale_ts(outputVidPacket, outputCtx->videoCodecCtx->time_base, outputCtx->videoStream->time_base);

            av_interleaved_write_frame(outputCtx->formatCtx, outputVidPacket);
        } else if (ret == AVERROR(EAGAIN)) {
            allDone = shouldStop;
        }

        if (avcodec_receive_packet(outputCtx->audioCodecCtx, outputAudPacket) == 0) {
            outputAudPacket->stream_index = outputCtx->audioIndex;
            av_packet_rescale_ts(outputAudPacket, outputCtx->audioCodecCtx->time_base, outputCtx->audioStream->time_base);

            av_interleaved_write_frame(outputCtx->formatCtx, outputAudPacket);
        }

        av_frame_unref(filteredResampledFrame);
        av_frame_unref(filteredAudFrame);
        av_frame_unref(input1AudFrame);
        av_frame_unref(input2AudFrame);

        av_frame_unref(filteredVidFrame);
        av_frame_unref(input1YuvFrame);
        av_frame_unref(input2YuvFrame);
        av_frame_unref(input1VidFrame);
        av_frame_unref(input2VidFrame);

        av_packet_unref(input1Packet);
        av_packet_unref(input2Packet);
        av_packet_unref(outputAudPacket);
        av_packet_unref(outputVidPacket);
    }

    // Write the trailer to the output file
    av_write_trailer(outputCtx->formatCtx);

    // Cleanup
    avformat_close_input(&input1Ctx->formatCtx);
    avformat_close_input(&input2Ctx->formatCtx);
    avformat_free_context(outputCtx->formatCtx);
    avformat_network_deinit();

    return 0;
}