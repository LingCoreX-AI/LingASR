/*
 * LingTTS 设备端推理库：token 序列 → 16 kHz PCM（流式回调）。
 *
 * - 不调用 malloc：所有缓冲从调用方提供的 arena 里分配，lingtts_init 时一次分配完
 * - 模型 lingtts.bin 由 export/dump_c.py 生成；设备上直接用 Flash mmap 的地址，库不拷贝权重
 * - head 之前全部是 INT8/INT32 整数运算，与 export/int_ref.py 逐位一致
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINGTTS_VERSION "0.2.0-rc2"

/* 剩余时长查询的无限制哨兵值，不是商业授权状态。 */
#define LINGTTS_UNLIMITED_SECONDS UINT32_MAX
#define LINGTTS_SAMPLE_RATE 16000
#define LINGTTS_CTX_DIM 6

enum {
    LINGTTS_OK = 0,
    LINGTTS_ERR_MODEL = -1,     /* 模型格式错误、版本不符或 CRC 错 */
    LINGTTS_ERR_ARENA = -2,     /* arena 不够 */
    LINGTTS_ERR_ARG = -3,
    LINGTTS_ERR_TOO_LONG = -4,  /* token 数超过 cfg.max_tokens，需要前端先分段 */
    LINGTTS_ERR_VOICE = -5,     /* 音色包格式错误、CRC 错或与基础模型不匹配 */
    LINGTTS_ERR_LICENSE = -6,   /* 授权无效、评估时长用完，或音色不在授权里 */
    LINGTTS_STOPPED = 1,        /* 回调要求停止 */
};

typedef struct {
    uint16_t n;                              /* token 数 */
    const uint8_t *phone, *tone, *pos;       /* 每个 token 一个 */
    const uint16_t (*ctx)[LINGTTS_CTX_DIM];  /* 6 维位置特征 */
} lingtts_tokens_t;

typedef struct {
    uint16_t block;        /* 帧级网络每块处理的帧数 B（默认 32） */
    uint16_t max_tokens;   /* 每段最多 token 数（默认 128） */
    float speed;           /* 语速：有限正数，默认 1.0，大于 1 更快，建议 0.5～2.0 */
    /* 可选的共享区（16 字节对齐）：每次合成时才用到的缓冲（工作区、esp-nn scratch、编码器输出、各层历史、
     * head / iSTFT 缓冲）从这里分配，只在 lingtts_synth_tokens 内部使用，两次调用之间可以被别人覆盖
     * （例如前端的 lingtts_fe_init_shared）。NULL = 全部从 arena 分配 */
    void *shared;
    size_t shared_size;
} lingtts_cfg_t;

/* 返回非 0 → 停止合成 */
typedef int (*lingtts_pcm_cb)(const int16_t *pcm, size_t n, void *user);

/* 调试钩子：每层输出（整数层为 int8，dur / head 为 int32，wav 为 float）。t0 为起始帧（wav 为起始采样点） */
enum {
    LT_DBG_X0, LT_DBG_ENC, LT_DBG_DUR, LT_DBG_D0, LT_DBG_DEC, LT_DBG_VIN, LT_DBG_VOC_INP, LT_DBG_VOC,
    LT_DBG_HEAD, LT_DBG_WAV,
};
typedef void (*lingtts_dbg_cb)(void *user, int kind, int index, int t0, int n, int ch, const void *data);

typedef struct {
    size_t arena_used;            /* arena 峰值（字节） */
    size_t shared_used;           /* 共享区用量（不用共享区时为 0） */
    size_t scratch_bytes;         /* 其中 esp-nn scratch + im2col 缓冲 */
    uint32_t frames, samples;     /* 最近一次合成 */
    uint64_t us_enc, us_dur, us_dec, us_voc, us_head, us_istft, us_total;   /* 最近一次合成的分模块耗时 */
} lingtts_stats_t;

typedef struct lingtts lingtts_t;

void lingtts_default_cfg(lingtts_cfg_t *cfg);

/* arena 需 16 字节对齐。*out 指向 arena 内部，无需释放。 */
int lingtts_init(lingtts_t **out, const void *model, size_t model_size, void *arena, size_t arena_size,
                 const lingtts_cfg_t *cfg);

/* 设置后续合成的语速倍率：必须是有限正数，默认 1.0，大于 1 更快。
 * 建议 0.5～2.0；极端倍率可能影响清晰度。调整音素时长，PCM 采样率仍为 16 kHz。
 * 不能在 lingtts_synth_tokens 进行中调用；不分配内存。失败返回 LINGTTS_ERR_ARG，保持原值。 */
int lingtts_set_speed(lingtts_t *tts, float speed);

int lingtts_synth_tokens(lingtts_t *tts, const lingtts_tokens_t *tok, lingtts_pcm_cb cb, void *user);

/* 切换音色：ltv 为音色包（.ltv，export/dump_voice.py 生成，16 字节对齐），NULL = 基础音色。
 * 音色包只替换少数几层的参数，权重直接指向 ltv（不拷贝），所以 ltv 在使用期间必须一直有效；
 * 不能在 lingtts_synth_tokens 进行中调用。失败时保持原来的音色。合成速度与基础模型相同。
 * 格式或签名错误返回 LINGTTS_ERR_VOICE，授权不允许这个音色返回 LINGTTS_ERR_LICENSE */
int lingtts_set_voice(lingtts_t *tts, const void *ltv, size_t size);

/* 可选设备凭证（license.bin，绑定设备 ID）。社区版无需此文件且不限合成时长。
 * 凭证状态只表示技术验签结果，不等于是否有商业使用权；商业范围以书面许可为准。
 * 有授权时只能加载签过名、且在授权音色列表里的音色包。失败返回 LINGTTS_ERR_LICENSE，原来的状态不变 */
int lingtts_set_license(lingtts_t *tts, const void *lic, size_t size);
int lingtts_is_licensed(const lingtts_t *tts);
/* 社区版返回 LINGTTS_UNLIMITED_SECONDS；内部限时构建返回本次开机剩余音频秒数。 */
uint32_t lingtts_eval_seconds_left(void);

/* 本机设备 ID（签发授权用）："U-" + 32 位十六进制（eFuse 唯一 ID）或 "M-" + 12 位十六进制（MAC）。
 * size 至少 35 */
int lingtts_device_id(char *buf, size_t size);

/* 库版本（库编译时的 LINGTTS_VERSION） */
const char *lingtts_version(void);

void lingtts_set_debug(lingtts_t *tts, lingtts_dbg_cb cb, void *user);
void lingtts_get_stats(const lingtts_t *tts, lingtts_stats_t *st);

#ifdef __cplusplus
}
#endif
