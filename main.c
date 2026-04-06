#include "rkmpp_dec.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char const *argv[])
{
    if (argc < 4) {
        printf("usage: %s input.h264 output.yuv type\n", argv[0]);
        printf("type: 7=H264(AVC), 167=H265(HEVC), 8=MJPEG ...\n");
        return -1;
    }

    printf("输入参数 码流文件:%s  目标文件:%s   码流压缩类型:%s\n",
           argv[1], argv[2], argv[3]);

    decode(argv[1], argv[2], (MppCodingType)atoi(argv[3]));
    return 0;
}
