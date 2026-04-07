#include "ffmpeg_pull_rtsp.h"
#include "media_utils.h"
#include "rkmpp_dec.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int ret;
    unsigned char *data = NULL;
    int size = 0;
    int frame_index = 0;
    int stream_inited = 0;
    int decoder_inited = 0;
    RkMppDecoder dec;
    FILE *f_out = NULL;

    ret = init_pull_rtsp_stream("rtsp://192.168.101.62:8554/live");
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize RTSP stream\n");
        return -1;
    }
    stream_inited = 1;

    f_out = fopen("output1111.yuv", "wb");
    if (!f_out) {
        perror("fopen output1111.yuv");
        goto end;
    }

    ret = rk_mpp_decoder_init(&dec, MPP_VIDEO_CodingAVC, f_out);
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize MPP decoder\n");
        goto end;
    }
    decoder_inited = 1;

    while (1) {
        H264InspectResult fmt_info;
        H264FrameType frame_type;

        ret = get__h264_data(&data, &size);
        if (ret < 0) {
            fprintf(stderr, "Failed to get H.264 data\n");
            break;
        }

        if (!data || size <= 0) {
            fprintf(stderr, "Invalid H.264 packet\n");
            release_h264_data();
            continue;
        }

        fmt_info = inspect_h264_packet(data, (size_t)size);
        frame_type = h264_packet_frame_type(data, (size_t)size);

        printf("[%06d] type=%s format=%s nalu_count=%d size=%d sps=%d pps=%d idr=%d",
               frame_index,
               h264_frame_type_name(frame_type),
               h264_format_name(fmt_info.format),
               fmt_info.nalu_count,
               size,
               fmt_info.has_sps,
               fmt_info.has_pps,
               fmt_info.has_idr);

        if (fmt_info.format == H264_FMT_AVCC) {
            printf(" avcc_length_size=%d", fmt_info.avcc_length_size);
        }
        printf("\n");
        fflush(stdout);

        ret = rk_mpp_decoder_send_data(&dec, data, size, 0);

        release_h264_data();
        data = NULL;
        size = 0;

        if (ret < 0) {
            fprintf(stderr, "Failed to send H.264 data to decoder\n");
            break;
        }

        frame_index++;
        printf("Frame %d sent to decoder successfully\n", frame_index);
    }

end:
    if (decoder_inited) {
        rk_mpp_decoder_deinit(&dec);
    }
    if (f_out) {
        fclose(f_out);
    }
    if (stream_inited) {
        clear_up();
    }

    return 0;
}
