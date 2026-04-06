#include "ffmpeg_pull_rtsp.h"
static void print_error(const char *msg, int err)
{
	char buf[256];
	av_strerror(err, buf, sizeof(buf));
	fprintf(stderr, "%s: %s\n", msg, buf);
}

typedef struct PullRtspContext {
    AVFormatContext *fmt_ctx;
    int video_index;
    AVPacket *pkt;
} PullRtspContext;
PullRtspContext ctx = {0};
int init_pull_rtsp_stream(const char *rtsp_url)
{
	
    int i;
    
	AVCodecParameters *video_acp = NULL;
	const char *codec_name = NULL;
	int n = 0;
	// 打开的这个输入整体 -------> ffmpeg -i input.mp4
	AVFormatContext *fmt_ctx = NULL;
	
	avformat_network_init();
	AVDictionary *opts = NULL;
	av_dict_set(&opts, "rtsp_transport", "tcp", 0);
	av_dict_set(&opts, "stimeout", "5000000", 0); // 5秒，单位微秒
	int ret = avformat_open_input(&fmt_ctx, rtsp_url,
								  NULL, &opts);
	if (ret < 0) {
		print_error("avformat_open_input", ret);
		return ret;
	}
	printf("rtsp连接成功\n");
	// 解析流 ffmpeg -i input.mp4 输出的stream的相关行
	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		print_error("avformat_find_stream_info", ret);
		goto err;
	}

	// 输出有几个流 video and audio
	printf("nb_stream = %d\n", fmt_ctx->nb_streams);

	// 遍历每个流
	for (i = 0; i < fmt_ctx->nb_streams; ++i) {
		// 获取每个流的指针
		AVStream *stream = fmt_ctx->streams[i];

		// 获取这个流的的参数
		AVCodecParameters *acp = stream->codecpar;
		const char *type;
		switch (acp->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
				/* code */
				type = "video";
				break;
			case AVMEDIA_TYPE_AUDIO:
				type = "audio";
				break;
			case AVMEDIA_TYPE_SUBTITLE:
				type = "subtitle";

			default:
				type = "other";
				break;
		}
		printf("stream #%u: type:%s codec_id=%d\n", i, type, acp->codec_id);
		if (acp->codec_type == AVMEDIA_TYPE_VIDEO) {
			// printf("视频流: stream#%u %u * %u\n", i, acp->width,
			// acp->height);
			ctx.video_index = i; // 拿视频流的索引
		}
	}
	if (ctx.video_index < 0) {
		fprintf(stderr, "no video stream\n");
		goto err;
	}
    ctx.pkt = av_packet_alloc();
    if (ctx.pkt == NULL) {
        fprintf(stderr, "av_packet_alloc failed\n");
        goto err;
    }
    ctx.fmt_ctx = fmt_ctx;
    video_acp = fmt_ctx->streams[ctx.video_index]->codecpar;  //拿到视频流的参数信息
    codec_name = avcodec_get_name(video_acp->codec_id);          //拿视频流的编码格式
    printf("video stream index:%d  codec_name:%s\n", ctx.video_index, codec_name);
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
        print_error("av_read_frame", ret);
        return ret;
    }
    if (ctx.pkt->stream_index != ctx.video_index) {
        av_packet_unref(ctx.pkt);
        return -1; // 不是视频流
    }
    *data = ctx.pkt->data;
    *size = ctx.pkt->size;
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
