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

bool shouldStop = false;
bool allDone = false;

void signalHandler(int signum) {
    if (signum == SIGINT) {
        std::cout << "signaled\n";
        shouldStop = true;
    }
}

int main() {
    std::signal(SIGINT, signalHandler);

    // Initialize FFmpeg
    avdevice_register_all();

    // Set screen capture parameters
    const char* outputFilename = "output.mp4";
    const char* pixelFormat = "uyvy422";
    // const AVCodecID outputCodecId = AV_CODEC_ID_H264;
    const int fps = 60;

    const int cropX = 100;
    const int cropY = 0;
    const int cropWidth = 500;
    const int cropHeight = 800;

    // Open screen capture input
    const AVInputFormat* inputFormat = nullptr;
// #ifdef __APPLE__
    inputFormat = av_find_input_format("avfoundation");
// #elif defined(__linux__)
//     inputFormat = av_find_input_format("x11grab");
// #endif

    // Create input1 context
    AVDictionary* options1 = nullptr;
    av_dict_set(&options1, "framerate", std::to_string(fps).c_str(), 0);
    av_dict_set(&options1, "pixel_format", pixelFormat, 0);
    av_dict_set(&options1, "capture_cursor", "1", 0);

    AVFormatContext* input1Context = nullptr;
    if (avformat_open_input(&input1Context, "2:", inputFormat, &options1) != 0) {
        std::cout << "Failed to open input\n";
        return 1;
    }

    if (avformat_find_stream_info(input1Context, nullptr) < 0) {
        std::cout << "Failed to find stream info\n";
        return 1;
    }

    // Find the video stream in the input
    int input1VideoStreamIndex = av_find_best_stream(input1Context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (input1VideoStreamIndex < 0) {
        std::cout << "Failed to find video stream\n";
        return 1;
    }

    AVStream *input1VideoStream = input1Context->streams[input1VideoStreamIndex];
    const AVCodec *input1Codec = avcodec_find_decoder(input1VideoStream->codecpar->codec_id);

    AVCodecContext* input1CodecContext = avcodec_alloc_context3(input1Codec);
    avcodec_parameters_to_context(input1CodecContext, input1VideoStream->codecpar);

    if (avcodec_open2(input1CodecContext, input1Codec, NULL) < 0) {
        std::cout << "Failed to open input codec\n";
        return 1;
    }

    // Create input2 context
    AVDictionary* options2 = nullptr;
    av_dict_set(&options2, "framerate", std::to_string(fps).c_str(), 0);
    av_dict_set(&options2, "pixel_format", pixelFormat, 0);
    av_dict_set(&options2, "capture_cursor", "1", 0);

    AVFormatContext* input2Context = nullptr;
    if (avformat_open_input(&input2Context, "3:", inputFormat, &options2) != 0) {
        std::cout << "Failed to open input\n";
        return 1;
    }

    if (avformat_find_stream_info(input2Context, nullptr) < 0) {
        std::cout << "Failed to find stream info\n";
        return 1;
    }

    // Find the video stream in the input
    int input2VideoStreamIndex = av_find_best_stream(input2Context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (input2VideoStreamIndex < 0) {
        std::cout << "Failed to find video stream\n";
        return 1;
    }

    AVStream *input2VideoStream = input2Context->streams[input2VideoStreamIndex];
    const AVCodec *input2Codec = avcodec_find_decoder(input2VideoStream->codecpar->codec_id);

    AVCodecContext* input2CodecContext = avcodec_alloc_context3(input2Codec);
    avcodec_parameters_to_context(input2CodecContext, input2VideoStream->codecpar);

    if (avcodec_open2(input2CodecContext, input2Codec, NULL) < 0) {
        std::cout << "Failed to open input codec\n";
        return 1;
    }

    // Create an output context
    AVFormatContext* outputContext = nullptr;
    avformat_alloc_output_context2(&outputContext, nullptr, nullptr, outputFilename);
    if (!outputContext) {
        std::cout << "Failed to create output context\n";
        return 1;
    }

    // Add a video stream to the output
    AVStream* outputVideoStream = avformat_new_stream(outputContext, nullptr);
    if (!outputVideoStream) {
        std::cout << "Failed to create output video stream\n";
        return 1;
    }

    // Find the video encoder
    const AVCodec* outputCodec = avcodec_find_encoder(outputContext->oformat->video_codec);
    if (!outputCodec) {
        std::cout << "Failed to find encoder: " << outputContext->video_codec_id << std::endl;
        return 1;
    }

    AVStream *outVideoStream = input1Context->streams[input1VideoStreamIndex];

    // Set codec parameters for the output video stream
    AVCodecContext* outCodecContext = avcodec_alloc_context3(outputCodec);
    outCodecContext->width = cropWidth * 2;
    outCodecContext->height = cropHeight;
    outCodecContext->sample_aspect_ratio = av_make_q(0, 1);
    outCodecContext->time_base = input1VideoStream->time_base;
    outCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avcodec_open2(outCodecContext, outputCodec, NULL) < 0) {
        std::cout << "Failed to open codec\n";
        return 1;
    }

    avcodec_parameters_from_context(outputVideoStream->codecpar, outCodecContext);

    // Open the output file
    if (avio_open(&outputContext->pb, outputFilename, AVIO_FLAG_WRITE) != 0) {
        std::cout << "Failed to open output file\n";
        return 1;
    }

    // Write the header to the output file
    if (avformat_write_header(outputContext, nullptr) != 0) {
        std::cout << "Failed to write output header\n";
        return 1;
    }

    SwsContext *swsInput1Ctx = sws_getContext(
        input1CodecContext->width,
        input1CodecContext->height,
        input1CodecContext->pix_fmt,
        input1CodecContext->width,
        input1CodecContext->height,
        outCodecContext->pix_fmt,
        SWS_BICUBIC,
        NULL,
        NULL,
        NULL
    );

    SwsContext *swsInput2Ctx = sws_getContext(
        input2CodecContext->width,
        input2CodecContext->height,
        input2CodecContext->pix_fmt,
        input2CodecContext->width,
        input2CodecContext->height,
        outCodecContext->pix_fmt,
        SWS_BICUBIC,
        NULL,
        NULL,
        NULL
    );

    int ret = 0;

    AVFilterGraph *filterGraph = avfilter_graph_alloc();

    AVFilterContext *bufferSrc1Ctx;
    AVFilterContext *bufferSrc2Ctx;
    AVFilterContext *crop1Ctx;
    AVFilterContext *crop2Ctx;
    AVFilterContext *padCtx;
    AVFilterContext *overlayCtx;
    AVFilterContext *bufferSinkCtx;

    const AVFilter *bufferSrcFilter = avfilter_get_by_name("buffer");
    const AVFilter *cropFilter = avfilter_get_by_name("crop");
    const AVFilter *padFilter = avfilter_get_by_name("pad");
    const AVFilter *overlayFilter = avfilter_get_by_name("overlay");
    const AVFilter *bufferSinkFilter = avfilter_get_by_name("buffersink");

    char filterArgs[512];
    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        input1CodecContext->width,
        input1CodecContext->height,
        outCodecContext->pix_fmt,
        outCodecContext->time_base.num,
        outCodecContext->time_base.den,
        input1CodecContext->sample_aspect_ratio.num,
        input1CodecContext->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&bufferSrc1Ctx, bufferSrcFilter, "in1", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        input2CodecContext->width,
        input2CodecContext->height,
        outCodecContext->pix_fmt,
        outCodecContext->time_base.num,
        outCodecContext->time_base.den,
        input2CodecContext->sample_aspect_ratio.num,
        input2CodecContext->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&bufferSrc2Ctx, bufferSrcFilter, "in2", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);

    ret = avfilter_graph_create_filter(&crop1Ctx, cropFilter, "crop1", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);

    ret = avfilter_graph_create_filter(&crop2Ctx, cropFilter, "crop2", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d", // w:h:x:y
        cropWidth * 2,
        cropHeight,
        0,
        0);

    ret = avfilter_graph_create_filter(&padCtx, padFilter, "pad", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d",
        cropWidth,
        0);

    ret = avfilter_graph_create_filter(&overlayCtx, overlayFilter, "overlay", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_graph_create_filter(&bufferSinkCtx, bufferSinkFilter, "out", nullptr, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(bufferSrc1Ctx, 0, crop1Ctx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(crop1Ctx, 0, padCtx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(padCtx, 0, overlayCtx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(bufferSrc2Ctx, 0, crop2Ctx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(crop2Ctx, 0, overlayCtx, 1);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(overlayCtx, 0, bufferSinkCtx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_graph_config(filterGraph, nullptr);
    if (ret < 0) {
        return ret;
    }

    // Read and encode frames
    AVPacket *input1Packet = av_packet_alloc();
    AVPacket *input2Packet = av_packet_alloc();
    AVFrame *input1Frame = av_frame_alloc();
    AVFrame *input2Frame = av_frame_alloc();
    AVFrame *yuv1Frame = av_frame_alloc();
    AVFrame *yuv2Frame = av_frame_alloc();
    AVPacket *outputPacket = av_packet_alloc();
    AVFrame *filteredFrame = av_frame_alloc();

    int64_t numFrames = -1;

    while (!allDone) {
        if (av_read_frame(input1Context, input1Packet) == 0) {
            if (input1Packet->stream_index == input1VideoStreamIndex) {
                avcodec_send_packet(input1CodecContext, input1Packet);
            }
        }

        if (av_read_frame(input2Context, input2Packet) == 0) {
            if (input2Packet->stream_index == input2VideoStreamIndex) {
                avcodec_send_packet(input2CodecContext, input2Packet);
            }
        }

        if (avcodec_receive_frame(input1CodecContext, input1Frame) == 0) {
            yuv1Frame->format = outCodecContext->pix_fmt;
            yuv1Frame->width = input1CodecContext->width;
            yuv1Frame->height = input1CodecContext->height;
            av_frame_get_buffer(yuv1Frame, 0);

            sws_scale(swsInput1Ctx,
                input1Frame->data,
                input1Frame->linesize,
                0,
                input1Frame->height,
                yuv1Frame->data,
                yuv1Frame->linesize
            );

            av_buffersrc_add_frame(bufferSrc1Ctx, yuv1Frame);
        }

        if (avcodec_receive_frame(input2CodecContext, input2Frame) == 0) {
            yuv2Frame->format = outCodecContext->pix_fmt;
            yuv2Frame->width = input2CodecContext->width;
            yuv2Frame->height = input2CodecContext->height;
            av_frame_get_buffer(yuv2Frame, 0);

            sws_scale(swsInput2Ctx,
                input2Frame->data,
                input2Frame->linesize,
                0,
                input2Frame->height,
                yuv2Frame->data,
                yuv2Frame->linesize
            );

            av_buffersrc_add_frame(bufferSrc2Ctx, yuv2Frame);
        }

        if (av_buffersink_get_frame(bufferSinkCtx, filteredFrame) == 0) {
            numFrames++;

            // Rescale timestamps
            filteredFrame->pts = av_rescale_rnd(numFrames, outCodecContext->time_base.den, fps, AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

            if (!shouldStop) {
                avcodec_send_frame(outCodecContext, filteredFrame);
            }
        }

        ret = avcodec_receive_packet(outCodecContext, outputPacket);
        if (ret == 0) {
            outputPacket->stream_index = outputVideoStream->index;
            av_packet_rescale_ts(outputPacket, outCodecContext->time_base, outputVideoStream->time_base);

            // Write the packet to the output file
            av_interleaved_write_frame(outputContext, outputPacket);
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
    av_write_trailer(outputContext);

    // Cleanup
    avformat_close_input(&input1Context);
    avformat_close_input(&input2Context);
    avformat_free_context(outputContext);
    av_dict_free(&options1);
    av_dict_free(&options2);
    avformat_network_deinit();

    return 0;
}