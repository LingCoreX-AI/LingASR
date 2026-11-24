/*
 * LingTTS 引擎：文本 → 16 kHz PCM 的一站式接口（大多数应用只需要这个头文件）。
 *
 * 引擎负责找资源、分配内存、前端分句和合成：
 *   lingtts_engine_cfg_t cfg;
 *   lingtts_engine_default_cfg(&cfg);
 *   lingtts_engine_t *e;
 *   if (lingtts_engine_create(&e, &cfg) == LINGTTS_OK) {
 *       lingtts_engine_speak(e, "你好，世界。", on_pcm, NULL);   // 阻塞，边合成边回调
 *   }
 *
 * 需要自己管理内存或调度的应用（比如和 ASR 分时复用内存）用下层的 lingtts.h 和 lingtts_frontend.h。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lingtts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* 工作区放 PSRAM，模型和词典从 Flash 复制进 PSRAM（约 2.2 MB，比直接读 Flash 快约 25%）。
     * 内部 RAM 只放引擎自身的几十字节 */
    LINGTTS_MEM_PSRAM = 0,
    /* 没有 PSRAM：工作区放内部 RAM，合成器和前端分时复用一块共享区（共约 149 KB），模型和词典直接读 Flash */
    LINGTTS_MEM_INTERNAL = 1,
} lingtts_mem_t;

#define LINGTTS_ENGINE_NO_COPY 1u   /* flags：PSRAM 模式也不复制资源，直接读 Flash（省 2.2 MB PSRAM，速度同 INTERNAL） */

typedef struct {
    /* 资源：NULL = 按名字找 data 分区并 mmap（"lingtts_model"、"lingtts_fe"；社区版不自动加载 "lingtts_lic"）。
     * 自己给的地址需 16 字节对齐，并在引擎销毁前一直有效（引擎不复制） */
    const void *model;
    size_t model_size;
    const void *frontend;
    size_t frontend_size;
    const void *license;          /* 可选显式设备凭证；社区版 NULL 时不限时且忽略授权分区 */
    size_t license_size;
    lingtts_mem_t mem;
    uint32_t flags;               /* LINGTTS_ENGINE_* */
    float speed;                  /* 语速：有限正数，默认 1.0，大于 1 更快，建议 0.5～2.0 */
    uint16_t block;               /* 帧级每块帧数，默认 32（越小首包越快、总耗时略长） */
    uint16_t max_tokens;          /* 每段最多 token 数，默认 128；更长的句子由前端在标点处切开 */
} lingtts_engine_cfg_t;

typedef struct lingtts_engine lingtts_engine_t;

/* 引擎占用的内存（create 成功后有效） */
typedef struct {
    size_t tts_arena, fe_arena, shared;    /* 合成器常驻、前端常驻、分时复用的共享区（PSRAM 模式为 0） */
    size_t copied;                          /* 复制进 PSRAM 的资源 */
} lingtts_engine_mem_t;

void lingtts_engine_default_cfg(lingtts_engine_cfg_t *cfg);

/* 返回 LINGTTS_OK、LINGTTS_ERR_MODEL（模型 / 词典格式错误或分区不存在）、LINGTTS_ERR_ARENA（内存不够）、
 * LINGTTS_ERR_LICENSE（有授权文件但无效：签名错、不是这台设备或已到期）、LINGTTS_ERR_ARG。
 * 设备凭证是否有效（不是商业使用权）：lingtts_is_licensed(lingtts_engine_tts(e))；本机设备 ID：lingtts_device_id() */
int lingtts_engine_create(lingtts_engine_t **out, const lingtts_engine_cfg_t *cfg);

/* 切换音色：ltv 为音色包（16 字节对齐，使用期间一直有效），NULL = 基础音色。失败时保持原来的音色。
 * 有授权时只接受签过名、且在授权音色列表里的音色包（否则 LINGTTS_ERR_LICENSE）。
 * 只能在调用 speak 的任务里、speak 之外调用 */
int lingtts_engine_set_voice(lingtts_engine_t *e, const void *ltv, size_t size);

/* 设置后续 speak 的语速：有限正数，1.0 = 正常，0.8 = 较慢，1.2 = 较快，建议 0.5～2.0。
 * 调整音素时长，输出仍是 16 kHz；不重建引擎、不分配内存。
 * 只能在调用 speak 的任务里、speak 之外调用。无效参数返回 LINGTTS_ERR_ARG，保持原值。 */
int lingtts_engine_set_speed(lingtts_engine_t *e, float speed);

/* 阻塞直到整段文本合成完：每产生一段 PCM 调一次 cb（cb 返回非 0 = 停止）。
 * 返回 LINGTTS_OK、LINGTTS_STOPPED（cb 要求停止或调用了 stop）或负的错误码；
 * 内部限时构建的评估时长用完时返回 LINGTTS_ERR_LICENSE（已经回调出去的音频不受影响） */
int lingtts_engine_speak(lingtts_engine_t *e, const char *utf8, lingtts_pcm_cb cb, void *user);

/* 可在任意任务调用：正在进行的 speak 在下一段 PCM 之前返回 LINGTTS_STOPPED（不再回调）。
 * 只影响当前这次 speak；没有 speak 在进行时什么也不做 */
void lingtts_engine_stop(lingtts_engine_t *e);

void lingtts_engine_get_mem(const lingtts_engine_t *e, lingtts_engine_mem_t *mem);

/* 合成器（调试 / 跑分用：lingtts_get_stats 等） */
lingtts_t *lingtts_engine_tts(lingtts_engine_t *e);

void lingtts_engine_destroy(lingtts_engine_t *e);

#ifdef __cplusplus
}
#endif
