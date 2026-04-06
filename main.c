#include "ffmpeg_pull_rtsp.h"
#include "rkmpp_dec.h"
#include <libavutil/error.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static void print_error(const char *msg, int err)
{
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", msg, buf);
}

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

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bitpos;
} BitReader;

/* =========================
 * 公共小工具
 * ========================= */
static int is_start_code3(const uint8_t *p, size_t left)
{
    return left >= 3 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01;
}

static int is_start_code4(const uint8_t *p, size_t left)
{
    return left >= 4 && p[0] == 0x00 && p[1] == 0x00 &&
           p[2] == 0x00 && p[3] == 0x01;
}

static uint32_t read_be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void mark_h264_nal_type(H264InspectResult *r, uint8_t nal_type)
{
    if (!r) {
        return;
    }

    if (nal_type == 7) {
        r->has_sps = 1;
    } else if (nal_type == 8) {
        r->has_pps = 1;
    } else if (nal_type == 5) {
        r->has_idr = 1;
    }
}

const char *h264_format_name(H264Format fmt)
{
    switch (fmt) {
    case H264_FMT_ANNEXB: return "Annex-B";
    case H264_FMT_AVCC:   return "AVCC";
    default:              return "UNKNOWN";
    }
}

const char *h264_frame_type_name(H264FrameType t)
{
    switch (t) {
    case H264_FRAME_I: return "I";
    case H264_FRAME_P: return "P";
    case H264_FRAME_B: return "B";
    default:           return "?";
    }
}

/* =========================
 * 判断封装格式
 * ========================= */
static int probe_annexb(const uint8_t *data, size_t size, H264InspectResult *result)
{
    size_t i = 0;
    int count = 0;

    while (i + 3 < size) {
        size_t sc_size = 0;
        size_t nalu_start, j;

        if (is_start_code4(data + i, size - i)) {
            sc_size = 4;
        } else if (is_start_code3(data + i, size - i)) {
            sc_size = 3;
        } else {
            i++;
            continue;
        }

        nalu_start = i + sc_size;
        j = nalu_start;

        while (j + 3 < size) {
            if (is_start_code4(data + j, size - j) ||
                is_start_code3(data + j, size - j)) {
                break;
            }
            j++;
        }

        if (nalu_start < size) {
            count++;
            mark_h264_nal_type(result, data[nalu_start] & 0x1F);
        }

        if (j + 3 >= size) {
            break;
        }
        i = j;
    }

    if (count > 0) {
        if (result) {
            result->nalu_count = count;
        }
        return 1;
    }
    return 0;
}

static int probe_avcc(const uint8_t *data, size_t size, int len_size, H264InspectResult *result)
{
    size_t pos = 0;
    int count = 0;

    if (!(len_size == 1 || len_size == 2 || len_size == 4)) {
        return 0;
    }

    while (pos + (size_t)len_size <= size) {
        uint32_t nalu_len = read_be(data + pos, len_size);
        pos += len_size;

        if (nalu_len == 0) {
            return 0;
        }
        if (pos + nalu_len > size) {
            return 0;
        }

        count++;
        mark_h264_nal_type(result, data[pos] & 0x1F);
        pos += nalu_len;
    }

    if (pos == size && count > 0) {
        if (result) {
            result->nalu_count = count;
        }
        return 1;
    }
    return 0;
}

H264InspectResult inspect_h264_packet(const uint8_t *data, size_t size)
{
    H264InspectResult r;
    r.format = H264_FMT_UNKNOWN;
    r.avcc_length_size = 0;
    r.nalu_count = 0;
    r.has_sps = 0;
    r.has_pps = 0;
    r.has_idr = 0;

    if (!data || size == 0) {
        return r;
    }

    if (probe_annexb(data, size, &r)) {
        r.format = H264_FMT_ANNEXB;
        return r;
    }

    if (probe_avcc(data, size, 4, &r)) {
        r.format = H264_FMT_AVCC;
        r.avcc_length_size = 4;
        return r;
    }
    if (probe_avcc(data, size, 2, &r)) {
        r.format = H264_FMT_AVCC;
        r.avcc_length_size = 2;
        return r;
    }
    if (probe_avcc(data, size, 1, &r)) {
        r.format = H264_FMT_AVCC;
        r.avcc_length_size = 1;
        return r;
    }

    return r;
}

/* =========================
 * 解析 slice type
 * ========================= */
static size_t nalu_to_rbsp(const uint8_t *src, size_t src_size,
                           uint8_t *dst, size_t dst_cap)
{
    size_t i = 0, j = 0;

    while (i < src_size && j < dst_cap) {
        if (i + 2 < src_size &&
            src[i] == 0x00 &&
            src[i + 1] == 0x00 &&
            src[i + 2] == 0x03) {
            if (j + 2 > dst_cap) {
                break;
            }
            dst[j++] = 0x00;
            dst[j++] = 0x00;
            i += 3;
            continue;
        }
        dst[j++] = src[i++];
    }
    return j;
}

static int br_read_bit(BitReader *br, uint32_t *bit)
{
    if (br->bitpos >= br->size * 8) {
        return -1;
    }
    *bit = (br->data[br->bitpos / 8] >> (7 - (br->bitpos % 8))) & 0x01;
    br->bitpos++;
    return 0;
}

static int br_read_bits(BitReader *br, int n, uint32_t *val)
{
    uint32_t v = 0, b;
    int i;

    for (i = 0; i < n; i++) {
        if (br_read_bit(br, &b) < 0) {
            return -1;
        }
        v = (v << 1) | b;
    }
    *val = v;
    return 0;
}

static int br_read_ue(BitReader *br, uint32_t *val)
{
    uint32_t zero_count = 0;
    uint32_t b;
    uint32_t suffix;

    while (1) {
        if (br_read_bit(br, &b) < 0) {
            return -1;
        }
        if (b == 1) {
            break;
        }
        zero_count++;
        if (zero_count > 31) {
            return -1;
        }
    }

    if (zero_count == 0) {
        *val = 0;
        return 0;
    }

    if (br_read_bits(br, (int)zero_count, &suffix) < 0) {
        return -1;
    }

    *val = ((1U << zero_count) - 1U) + suffix;
    return 0;
}

static int parse_slice_type_from_nalu(const uint8_t *nalu, size_t nalu_size,
                                      uint32_t *slice_type_out)
{
    uint8_t rbsp[4096];
    size_t rbsp_size;
    BitReader br;
    uint32_t first_mb_in_slice, slice_type;

    if (!nalu || nalu_size < 2) {
        return -1;
    }

    rbsp_size = nalu_to_rbsp(nalu + 1, nalu_size - 1, rbsp, sizeof(rbsp));
    if (rbsp_size == 0) {
        return -1;
    }

    br.data = rbsp;
    br.size = rbsp_size;
    br.bitpos = 0;

    if (br_read_ue(&br, &first_mb_in_slice) < 0) {
        return -1;
    }
    if (br_read_ue(&br, &slice_type) < 0) {
        return -1;
    }

    *slice_type_out = slice_type;
    return 0;
}

static H264FrameType slice_type_to_frame_type(uint32_t slice_type)
{
    slice_type %= 5;
    if (slice_type == 0) return H264_FRAME_P;
    if (slice_type == 1) return H264_FRAME_B;
    if (slice_type == 2) return H264_FRAME_I;
    return H264_FRAME_UNKNOWN;
}

static const uint8_t *find_annexb_nalu(const uint8_t *data, size_t size, size_t *nalu_size)
{
    size_t i = 0;

    while (i + 3 < size) {
        size_t sc_size = 0;
        size_t start, j;

        if (is_start_code4(data + i, size - i)) {
            sc_size = 4;
        } else if (is_start_code3(data + i, size - i)) {
            sc_size = 3;
        } else {
            i++;
            continue;
        }

        start = i + sc_size;
        j = start;
        while (j + 3 < size) {
            if (is_start_code4(data + j, size - j) ||
                is_start_code3(data + j, size - j)) {
                break;
            }
            j++;
        }

        if (start < size) {
            *nalu_size = (j + 3 < size) ? (j - start) : (size - start);
            return data + start;
        }

        i = j;
    }

    return NULL;
}

static const uint8_t *find_avcc_nalu(const uint8_t *data, size_t size,
                                     int len_size, size_t *nalu_size)
{
    uint32_t len;

    if (size < (size_t)len_size + 1) {
        return NULL;
    }
    if (!(len_size == 1 || len_size == 2 || len_size == 4)) {
        return NULL;
    }

    len = read_be(data, len_size);
    if (len == 0 || (size_t)len_size + len > size) {
        return NULL;
    }

    *nalu_size = len;
    return data + len_size;
}

H264FrameType h264_packet_frame_type(const uint8_t *data, size_t size)
{
    const uint8_t *nalu = NULL;
    size_t nalu_size = 0;
    uint8_t nal_type;
    uint32_t slice_type;

    if (!data || size < 2) {
        return H264_FRAME_UNKNOWN;
    }

    /* Annex-B */
    {
        size_t pos = 0;
        while (pos + 4 < size) {
            size_t cur_size = 0;
            const uint8_t *cur = find_annexb_nalu(data + pos, size - pos, &cur_size);
            if (!cur || cur_size < 1) {
                break;
            }

            nal_type = cur[0] & 0x1F;
            if (nal_type == 5) {
                return H264_FRAME_I;
            }
            if (nal_type == 1) {
                if (parse_slice_type_from_nalu(cur, cur_size, &slice_type) == 0) {
                    return slice_type_to_frame_type(slice_type);
                }
                return H264_FRAME_UNKNOWN;
            }

            pos = (size_t)(cur - data) + cur_size;
        }
    }

    /* AVCC */
    {
        int lens[3] = {4, 2, 1};
        int i;
        for (i = 0; i < 3; i++) {
            nalu = find_avcc_nalu(data, size, lens[i], &nalu_size);
            if (!nalu || nalu_size < 1) {
                continue;
            }

            nal_type = nalu[0] & 0x1F;
            if (nal_type == 5) {
                return H264_FRAME_I;
            }
            if (nal_type == 1) {
                if (parse_slice_type_from_nalu(nalu, nalu_size, &slice_type) == 0) {
                    return slice_type_to_frame_type(slice_type);
                }
                return H264_FRAME_UNKNOWN;
            }
        }
    }

    return H264_FRAME_UNKNOWN;
}

/* =========================
 * main
 * ========================= */
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

        // /* 无论成功失败，这次拿到的数据都先释放 */
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
