#include "ffmpeg_pull_rtsp.h"

static void print_error(const char *msg, int err)
{
	char buf[256];
	av_strerror(err, buf, sizeof(buf));
	fprintf(stderr, "%s: %s\n", msg, buf);
}

static int is_start_code3(const uint8_t *p, int left)
{
    return left >= 3 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01;
}

static int is_start_code4(const uint8_t *p, int left)
{
    return left >= 4 && p[0] == 0x00 && p[1] == 0x00 &&
           p[2] == 0x00 && p[3] == 0x01;
}

static void print_hex_prefix(const uint8_t *data, int size, int max_bytes)
{
    int i;
    int limit = size < max_bytes ? size : max_bytes;

    for (i = 0; i < limit; i++) {
        printf("%02x", data[i]);
        if (i + 1 < limit) {
            printf(" ");
        }
    }
    if (size > limit) {
        printf(" ...");
    }
    printf("\n");
}

static void print_h264_extradata_info(const AVCodecParameters *video_acp)
{
    const uint8_t *data = video_acp->extradata;
    int size = video_acp->extradata_size;
    int has_sps = 0;
    int has_pps = 0;
    int length_size = 0;

    printf("codec extradata_size=%d\n", size);
    if (!data || size <= 0) {
        printf("codec extradata: empty\n");
        return;
    }

    printf("codec extradata prefix: ");
    print_hex_prefix(data, size, 32);

    if (size >= 6 && data[0] == 1) {
        int pos;
        int i;
        int sps_count = data[5] & 0x1F;
        int pps_count = 0;

        length_size = (data[4] & 0x03) + 1;
        has_sps = sps_count > 0;
        pos = 6;

        for (i = 0; i < sps_count; i++) {
            int sps_len;

            if (pos + 2 > size) {
                break;
            }
            sps_len = (data[pos] << 8) | data[pos + 1];
            pos += 2 + sps_len;
            if (pos > size) {
                break;
            }
        }

        if (pos < size) {
            pps_count = data[pos];
            has_pps = pps_count > 0;
        }

        printf("codec extradata format=AVCC length_size=%d sps=%d pps=%d\n",
               length_size, has_sps, has_pps);
        return;
    }

    if (is_start_code3(data, size) || is_start_code4(data, size)) {
        int i = 0;

        while (i + 3 < size) {
            int sc_size = 0;
            int nalu_start;

            if (is_start_code4(data + i, size - i)) {
                sc_size = 4;
            } else if (is_start_code3(data + i, size - i)) {
                sc_size = 3;
            } else {
                i++;
                continue;
            }

            nalu_start = i + sc_size;
            if (nalu_start < size) {
                uint8_t nal_type = data[nalu_start] & 0x1F;
                if (nal_type == 7) {
                    has_sps = 1;
                } else if (nal_type == 8) {
                    has_pps = 1;
                }
            }
            i = nalu_start;
        }

        printf("codec extradata format=Annex-B sps=%d pps=%d\n",
               has_sps, has_pps);
        return;
    }

    printf("codec extradata format=UNKNOWN sps=%d pps=%d\n",
           has_sps, has_pps);
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
        print_error("av_read_frame", ret);
        return ret;
    }
    if (ctx.pkt->stream_index != ctx.video_index) {
        av_packet_unref(ctx.pkt);
        return -1; // 不是视频流
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
