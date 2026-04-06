#include "media_utils.h"

#include <libavutil/error.h>
#include <stdio.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bitpos;
} BitReader;

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

static void mark_h264_nal_type(H264InspectResult *result, uint8_t nal_type)
{
    if (!result) {
        return;
    }

    if (nal_type == 7) {
        result->has_sps = 1;
    } else if (nal_type == 8) {
        result->has_pps = 1;
    } else if (nal_type == 5) {
        result->has_idr = 1;
    }
}

static int probe_annexb(const uint8_t *data, size_t size, H264InspectResult *result)
{
    size_t i = 0;
    int count = 0;

    while (i + 3 < size) {
        size_t sc_size = 0;
        size_t nalu_start;
        size_t j;

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

static int probe_avcc(const uint8_t *data, size_t size, int len_size,
                      H264InspectResult *result)
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

static size_t nalu_to_rbsp(const uint8_t *src, size_t src_size,
                           uint8_t *dst, size_t dst_cap)
{
    size_t i = 0;
    size_t j = 0;

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
    uint32_t v = 0;
    uint32_t b;
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
    uint32_t first_mb_in_slice;
    uint32_t slice_type;

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

static const uint8_t *find_annexb_nalu(const uint8_t *data, size_t size,
                                       size_t *nalu_size)
{
    size_t i = 0;

    while (i + 3 < size) {
        size_t sc_size = 0;
        size_t start;
        size_t j;

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

void print_av_error(const char *msg, int err)
{
    char buf[256];

    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", msg, buf);
}

void print_h264_extradata_info(const AVCodecParameters *video_acp)
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

const char *h264_format_name(H264Format fmt)
{
    switch (fmt) {
    case H264_FMT_ANNEXB: return "Annex-B";
    case H264_FMT_AVCC:   return "AVCC";
    default:              return "UNKNOWN";
    }
}

const char *h264_frame_type_name(H264FrameType type)
{
    switch (type) {
    case H264_FRAME_I: return "I";
    case H264_FRAME_P: return "P";
    case H264_FRAME_B: return "B";
    default:           return "?";
    }
}

H264InspectResult inspect_h264_packet(const uint8_t *data, size_t size)
{
    H264InspectResult result;

    result.format = H264_FMT_UNKNOWN;
    result.avcc_length_size = 0;
    result.nalu_count = 0;
    result.has_sps = 0;
    result.has_pps = 0;
    result.has_idr = 0;

    if (!data || size == 0) {
        return result;
    }

    if (probe_annexb(data, size, &result)) {
        result.format = H264_FMT_ANNEXB;
        return result;
    }

    if (probe_avcc(data, size, 4, &result)) {
        result.format = H264_FMT_AVCC;
        result.avcc_length_size = 4;
        return result;
    }
    if (probe_avcc(data, size, 2, &result)) {
        result.format = H264_FMT_AVCC;
        result.avcc_length_size = 2;
        return result;
    }
    if (probe_avcc(data, size, 1, &result)) {
        result.format = H264_FMT_AVCC;
        result.avcc_length_size = 1;
        return result;
    }

    return result;
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
