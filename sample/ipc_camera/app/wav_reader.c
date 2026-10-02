#include "wav_reader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 有界加载，避免错误文件占满板端内存：最多4 MiB且不超过60秒。 */
#define WAV_MAX_DATA (4U * 1024U * 1024U)

static unsigned int read_le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int supported_rate(unsigned int rate)
{
    return rate == 8000U || rate == 11025U || rate == 12000U ||
           rate == 16000U || rate == 22050U || rate == 24000U ||
           rate == 32000U || rate == 44100U || rate == 48000U;
}

int wav_pcm_read(FILE *stream, WavPcm *pcm)
{
    unsigned char header[12], chunk[8], format[16];
    uint64_t end, position, next;
    uint32_t data_size = 0U;
    long file_size, data_offset = -1;
    unsigned int rate = 0U;
    int have_format = 0;

    if (stream == NULL || pcm == NULL) {
        return -1;
    }
    memset(pcm, 0, sizeof(*pcm));
    if (fseek(stream, 0, SEEK_END) != 0 || (file_size = ftell(stream)) < 12 ||
        fseek(stream, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), stream) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return -1;
    }
    end = (uint64_t)read_le32(header + 4) + 8U;
    if (end < 12U || end > (uint64_t)file_size) {
        return -1;
    }

    /* WAV头不一定是44字节：逐块解析fmt/data，跳过LIST/JUNK等元数据。 */
    for (position = 12U; position < end; position = next) {
        uint32_t size;
        if (end - position < 8U || fseek(stream, (long)position, SEEK_SET) != 0 ||
            fread(chunk, 1, sizeof(chunk), stream) != sizeof(chunk)) {
            return -1;
        }
        size = read_le32(chunk + 4);
        /* 奇数长度chunk后有一个对齐字节；所有计算用64位防止整数回绕。 */
        next = position + 8U + (uint64_t)size + (size & 1U);
        if (next > end) {
            return -1;
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (have_format || size < sizeof(format) ||
                fread(format, 1, sizeof(format), stream) != sizeof(format)) {
                return -1;
            }
            rate = read_le32(format + 4);
            /* 拒绝浮点/压缩/立体声，避免猜测格式导致噪声或倍速播放。 */
            if (read_le16(format) != 1U || read_le16(format + 2) != 1U ||
                read_le16(format + 14) != 16U || !supported_rate(rate) ||
                read_le16(format + 12) != 2U || read_le32(format + 8) != rate * 2U) {
                return -1;
            }
            have_format = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (data_offset >= 0 || size == 0U || size > WAV_MAX_DATA || (size & 1U)) {
                return -1;
            }
            data_offset = (long)(position + 8U);
            data_size = size;
        }
    }
    if (!have_format || data_offset < 0 || data_size > (uint64_t)rate * 2U * 60U) {
        return -1;
    }
    pcm->data = malloc(data_size);
    if (pcm->data == NULL || fseek(stream, data_offset, SEEK_SET) != 0 ||
        fread(pcm->data, 1, data_size, stream) != data_size) {
        wav_pcm_free(pcm);
        return -1;
    }
    pcm->size = data_size;
    pcm->sample_rate = rate;
    return 0;
}

void wav_pcm_free(WavPcm *pcm)
{
    if (pcm != NULL) {
        free(pcm->data);
        memset(pcm, 0, sizeof(*pcm));
    }
}
