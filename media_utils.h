#ifndef __MEDIA_UTILS_H__
#define __MEDIA_UTILS_H__

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

typedef enum {
    H264_FMT_UNKNOWN = 0,
    H264_FMT_ANNEXB,
    H264_FMT_AVCC
} H264Format;

typedef struct {
    H264Format format;
    int avcc_length_size;
    int nalu_count;
    int has_sps;
    int has_pps;
    int has_idr;
} H264InspectResult;

typedef enum {
    H264_FRAME_UNKNOWN = 0,
    H264_FRAME_I,
    H264_FRAME_P,
    H264_FRAME_B
} H264FrameType;

void print_av_error(const char *msg, int err);
void print_h264_extradata_info(const AVCodecParameters *video_acp);

const char *h264_format_name(H264Format fmt);
const char *h264_frame_type_name(H264FrameType type);
H264InspectResult inspect_h264_packet(const uint8_t *data, size_t size);
H264FrameType h264_packet_frame_type(const uint8_t *data, size_t size);

#endif  // __MEDIA_UTILS_H__
