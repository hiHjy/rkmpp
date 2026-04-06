#ifndef __RKMPP_DEC_H__
#define __RKMPP_DEC_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "mpp_common.h"
#include "mpp_mem.h"
#include "mpp_time.h"
#include "rk_mpi.h"

typedef struct RkMppDecoder_t {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppDecCfg dec_cfg;
    MppFrame frame;
    MppBufferGroup frm_grp;
    FILE *f_out;
    int frame_count;
    int timeout_count;
    int eos_wait_count;
    int eos_sent;
} RkMppDecoder;

const char *get_mpp_frame_fmt_name(RK_U32 fmt);
void dump_frame_nv12(MppFrame frame, FILE *fp_out);
int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out);
int rk_mpp_decoder_handle_info_change(RkMppDecoder *dec, MppFrame frame);
int rk_mpp_decoder_handle_frame(RkMppDecoder *dec, MppFrame frame);
int rk_mpp_decoder_poll_frames(RkMppDecoder *dec);
int rk_mpp_decoder_send_data(RkMppDecoder *dec, uint8_t *data, size_t len, int eos);
int rk_mpp_decoder_run_file(RkMppDecoder *dec, FILE *f_in);
void rk_mpp_decoder_deinit(RkMppDecoder *dec);
void decode(const char *input, const char *output, MppCodingType type);

#endif  // __RKMPP_DEC_H__
