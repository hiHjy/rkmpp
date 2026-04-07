#include "ffmpeg_pull_rtsp.h"
#include "media_utils.h"

#include <stdio.h>

typedef struct PullRtspContext {
    AVFormatContext *fmt_ctx;
    int video_index;
    AVPacket *pkt;
} PullRtspContext;

static PullRtspContext ctx = {0};

int init_pull_rtsp_stream(const char *rtsp_url)
{
    int i;
    AVCodecParameters *video_acp = NULL;
    const char *codec_name = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    int ret;

    avformat_network_init();
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);

    ret = avformat_open_input(&fmt_ctx, rtsp_url, NULL, &opts);
    if (ret < 0) {
        print_av_error("avformat_open_input", ret);
        return ret;
    }

    printf("rtsp连接成功\n");

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        print_av_error("avformat_find_stream_info", ret);
        goto err;
    }

    printf("nb_stream = %d\n", fmt_ctx->nb_streams);

    for (i = 0; i < fmt_ctx->nb_streams; ++i) {
        AVStream *stream = fmt_ctx->streams[i];
        AVCodecParameters *acp = stream->codecpar;
        const char *type;

        switch (acp->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            type = "video";
            break;
        case AVMEDIA_TYPE_AUDIO:
            type = "audio";
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            type = "subtitle";
            break;
        default:
            type = "other";
            break;
        }

        printf("stream #%u: type:%s codec_id=%d\n", i, type, acp->codec_id);
        if (acp->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx.video_index = i;
        }
    }

    if (ctx.video_index < 0) {
        fprintf(stderr, "no video stream\n");
        goto err;
    }

    ctx.pkt = av_packet_alloc();
    if (!ctx.pkt) {
        fprintf(stderr, "av_packet_alloc failed\n");
        goto err;
    }

    ctx.fmt_ctx = fmt_ctx;
    video_acp = fmt_ctx->streams[ctx.video_index]->codecpar;
    codec_name = avcodec_get_name(video_acp->codec_id);
    printf("video stream index:%d  codec_name:%s\n", ctx.video_index, codec_name);

    if (video_acp->codec_id == AV_CODEC_ID_H264) {
        print_h264_extradata_info(video_acp);
    }

    return 0;

err:
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    if (ctx.pkt) {
        av_packet_free(&ctx.pkt);
    }
    avformat_network_deinit();
    return ret;
}

int get__h264_data(uint8_t **data, int *size)
{
    int ret = av_read_frame(ctx.fmt_ctx, ctx.pkt);

    if (ret < 0) {
        print_av_error("av_read_frame", ret);
        return ret;
    }

    if (ctx.pkt->stream_index != ctx.video_index) {
        av_packet_unref(ctx.pkt);
        return -1;
    }

    *data = ctx.pkt->data;
    *size = ctx.pkt->size;
    printf("got video packet size=%d\n", *size);
    return 0;
}

void release_h264_data()
{
    av_packet_unref(ctx.pkt);
}

void clear_up()
{
    if (ctx.fmt_ctx) {
        avformat_close_input(&ctx.fmt_ctx);
        ctx.fmt_ctx = NULL;
    }

    if (ctx.pkt) {
        av_packet_free(&ctx.pkt);
        ctx.pkt = NULL;
    }

    avformat_network_deinit();
}
