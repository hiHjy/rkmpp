#ifndef __FFMPEG_PULL_RTSP_H__
#define __FFMPEG_PULL_RTSP_H__
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
int init_pull_rtsp_stream(const char *rtsp_url);
int get__h264_data(uint8_t **data, int *size);
void release_h264_data();
void clear_up();
#endif  // __FFMPEG_PULL_RTSP_H__