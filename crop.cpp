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
    const int cropY = 100;
    const int cropWidth = 900;
    const int cropHeight = 400;

    // Open screen capture input
    const AVInputFormat* inputFormat = nullptr;
// #ifdef __APPLE__
    inputFormat = av_find_input_format("avfoundation");
// #elif defined(__linux__)
//     inputFormat = av_find_input_format("x11grab");
// #endif

    AVDictionary* options = nullptr;
    av_dict_set(&options, "framerate", std::to_string(fps).c_str(), 0);
    av_dict_set(&options, "pixel_format", pixelFormat, 0);

    AVFormatContext* inputContext = nullptr;
    if (avformat_open_input(&inputContext, "2:", inputFormat, &options) != 0) {
        std::cout << "Failed to open input\n";
        return 1;
    }

    if (avformat_find_stream_info(inputContext, nullptr) < 0) {
        std::cout << "Failed to find stream info\n";
        return 1;
    }

    // Find the video stream in the input
    int videoStreamIndex = av_find_best_stream(inputContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIndex < 0) {
        std::cout << "Failed to find video stream\n";
        avformat_close_input(&inputContext);
        return 1;
    }

    AVStream *inputVideoStream = inputContext->streams[videoStreamIndex];
    const AVCodec *inputCodec = avcodec_find_decoder(inputVideoStream->codecpar->codec_id);

    AVCodecContext* inputCodecContext = avcodec_alloc_context3(inputCodec);
    avcodec_parameters_to_context(inputCodecContext, inputVideoStream->codecpar);

    if (avcodec_open2(inputCodecContext, inputCodec, NULL) < 0) {
        std::cout << "Failed to open input codec\n";
        return 1;
    }

    // Create an output context
    AVFormatContext* outputContext = nullptr;
    avformat_alloc_output_context2(&outputContext, nullptr, nullptr, outputFilename);
    if (!outputContext) {
        std::cout << "Failed to create output context\n";
        avformat_close_input(&inputContext);
        return 1;
    }

    // Add a video stream to the output
    AVStream* outputVideoStream = avformat_new_stream(outputContext, nullptr);
    if (!outputVideoStream) {
        std::cout << "Failed to create output video stream\n";
        avformat_close_input(&inputContext);
        avformat_free_context(outputContext);
        return 1;
    }

    // Find the video encoder
    const AVCodec* codec = avcodec_find_encoder(outputContext->oformat->video_codec);
    if (!codec) {
        std::cout << "Failed to find encoder: " << outputContext->video_codec_id << std::endl;
        avformat_close_input(&inputContext);
        return 1;
    }

    AVStream *outVideoStream = inputContext->streams[videoStreamIndex];

    // Set codec parameters for the output video stream
    AVCodecContext* outCodecContext = avcodec_alloc_context3(codec);
    outCodecContext->width = cropWidth;
    outCodecContext->height = cropHeight;
    outCodecContext->sample_aspect_ratio = inputVideoStream->sample_aspect_ratio;
    outCodecContext->time_base = inputVideoStream->time_base;
    outCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avcodec_open2(outCodecContext, codec, NULL) < 0) {
        std::cout << "Failed to open codec\n";
        avformat_close_input(&inputContext);
        avformat_free_context(outputContext);
        return 1;
    }

    avcodec_parameters_from_context(outputVideoStream->codecpar, outCodecContext);

    // Open the output file
    if (avio_open(&outputContext->pb, outputFilename, AVIO_FLAG_WRITE) != 0) {
        std::cout << "Failed to open output file\n";
        avformat_close_input(&inputContext);
        avformat_free_context(outputContext);
        return 1;
    }

    // Write the header to the output file
    if (avformat_write_header(outputContext, nullptr) != 0) {
        std::cout << "Failed to write output header\n";
        avformat_close_input(&inputContext);
        avformat_free_context(outputContext);
        return 1;
    }

    SwsContext *swsContext = sws_getContext(
        inputCodecContext->width,
        inputCodecContext->height,
        inputCodecContext->pix_fmt,
        inputCodecContext->width,
        inputCodecContext->height,
        outCodecContext->pix_fmt,
        SWS_BICUBIC,
        NULL,
        NULL,
        NULL
    );

    int ret = 0;

    AVFilterGraph *filterGraph = avfilter_graph_alloc();

    AVFilterContext *cropCtx;
    AVFilterContext *bufferSinkCtx;
    AVFilterContext *bufferSrcCtx;

    const AVFilter *cropFilter = avfilter_get_by_name("crop");
    const AVFilter *bufferSrcFilter = avfilter_get_by_name("buffer");
    const AVFilter *bufferSinkFilter = avfilter_get_by_name("buffersink");

    char filterArgs[512];
    snprintf(filterArgs, sizeof(filterArgs),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        inputCodecContext->width,
        inputCodecContext->height,
        outCodecContext->pix_fmt,
        outCodecContext->time_base.num,
        outCodecContext->time_base.den,
        outCodecContext->sample_aspect_ratio.num,
        outCodecContext->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&bufferSrcCtx, bufferSrcFilter, "in", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_graph_create_filter(&bufferSinkCtx, bufferSinkFilter, "out", nullptr, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    snprintf(filterArgs, sizeof(filterArgs),
        "%d:%d:%d:%d",
        cropWidth,
        cropHeight,
        cropX,
        cropY);

    ret = avfilter_graph_create_filter(&cropCtx, cropFilter, "crop", filterArgs, nullptr, filterGraph);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(bufferSrcCtx, 0, cropCtx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_link(cropCtx, 0, bufferSinkCtx, 0);
    if (ret < 0) {
        return ret;
    }

    ret = avfilter_graph_config(filterGraph, nullptr);
    if (ret < 0) {
        return ret;
    }

    // Read and encode frames
    AVPacket *inputPacket = av_packet_alloc();
    AVPacket *outputPacket = av_packet_alloc();
    AVFrame *inputFrame = av_frame_alloc();
    AVFrame *yuvFrame = av_frame_alloc();
    AVFrame *filteredFrame = av_frame_alloc();

    int64_t numFrames = -1;
    int64_t initialPts = 0;

    while (!allDone && ret >= 0) {
        ret = av_read_frame(inputContext, inputPacket);
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;
            continue;
        } else if (ret == AVERROR_EOF) {
            break;
        }

        if (inputPacket->stream_index != videoStreamIndex) {
            continue;
        }

        numFrames++;

        int ret2 = avcodec_send_packet(inputCodecContext, inputPacket);
        while (ret2 >= 0) {
            ret2 = avcodec_receive_frame(inputCodecContext, inputFrame);
            if (ret2 == AVERROR(EAGAIN) || ret2 == AVERROR_EOF) {
                break;
            }

            yuvFrame->format = outCodecContext->pix_fmt;
            yuvFrame->width = inputCodecContext->width;
            yuvFrame->height = inputCodecContext->height;
            av_frame_get_buffer(yuvFrame, 0);

            sws_scale(swsContext,
                inputFrame->data,
                inputFrame->linesize,
                0,
                inputFrame->height,
                yuvFrame->data,
                yuvFrame->linesize
            );

            int ret3 = av_buffersrc_add_frame(bufferSrcCtx, yuvFrame);
            while (ret3 >= 0) {
                ret3 = av_buffersink_get_frame(bufferSinkCtx, filteredFrame);
                if (ret3 == AVERROR(EAGAIN) || ret3 == AVERROR_EOF) {
                    break;
                }

                // Rescale timestamps
                filteredFrame->pts = av_rescale_rnd(numFrames, inputVideoStream->time_base.den, fps, AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

                int ret4 = 0;
                if (!shouldStop) {
                    ret4 = avcodec_send_frame(outCodecContext, filteredFrame);
                }

                while (ret4 >= 0) {
                    ret4 = avcodec_receive_packet(outCodecContext, outputPacket);
                    if (ret4 == AVERROR(EAGAIN)) {
                        allDone = shouldStop;
                        break;
                    }

                    outputPacket->stream_index = outputVideoStream->index;
                    av_packet_rescale_ts(outputPacket, inputVideoStream->time_base, outputVideoStream->time_base);

                    // Write the packet to the output file
                    av_interleaved_write_frame(outputContext, outputPacket);

                    av_packet_unref(outputPacket);
                }
            }

            av_frame_unref(filteredFrame);
            av_frame_unref(yuvFrame);
            av_frame_unref(inputFrame);
        }

        av_packet_unref(inputPacket);
    }

    // Write the trailer to the output file
    av_write_trailer(outputContext);

    // Cleanup
    avformat_close_input(&inputContext);
    avformat_free_context(outputContext);
    av_dict_free(&options);
    avformat_network_deinit();

    return 0;
}