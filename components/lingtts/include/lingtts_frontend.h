/*
 * LingTTS 设备端文本前端：UTF-8 文本 → 正则化 → 分词、读音、变调、韵律特征 → token（lingtts_tokens_t）。
 * 与 Python 版（frontend/tn/rules.py + WeTextProcessing + frontend/g2p/g2p.py）的输出逐 token 对齐。
 * 数据 lingtts_fe.bin 由 export/dump_frontend.py 生成，设备上直接用 Flash mmap 的地址。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lingtts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINGTTS_FE_MAX_CHARS 256     /* 每句（正则化前后）最多码位数；更长的句子由 lingtts_fe_text 在标点处切开 */
#define LINGTTS_FE_MAX_TOKENS 600    /* 每句最多 token 数 */

typedef struct lingtts_fe lingtts_fe_t;

/* 每句的结果（指向前端内部缓冲，下次调用前有效） */
typedef struct {
    lingtts_tokens_t tok;
    int n_syl;
    const uint16_t *syl;             /* 每个汉字的带调音节 id（调试 / 测试用） */
} lingtts_fe_sent_t;

size_t lingtts_fe_arena_size(void);
int lingtts_fe_init(lingtts_fe_t **out, const void *blob, size_t size, void *arena, size_t arena_size);

/* 分时复用（没有 PSRAM 时省内存）：正则化工作区和每句临时缓冲放在 shared 里。
 * 前端只在 lingtts_fe_normalize / lingtts_fe_g2p / lingtts_fe_segment 内部，以及 lingtts_fe_text 处理每句
 * 的正则化和 G2P 时使用 shared，不跨越回调；所以 shared 可以和合成器的 cfg.shared 是同一块内存。
 * arena 至少 lingtts_fe_arena_size_split()，shared 至少 lingtts_fe_shared_size()（都需 16 字节对齐）。 */
size_t lingtts_fe_arena_size_split(void);
size_t lingtts_fe_shared_size(void);
int lingtts_fe_init_shared(lingtts_fe_t **out, const void *blob, size_t size, void *arena, size_t arena_size,
                           void *shared, size_t shared_size);

/* 已正则化的一句 → token（等价于 Python 的 g2p(norm)） */
int lingtts_fe_g2p(lingtts_fe_t *fe, const char *norm_utf8, lingtts_fe_sent_t *out);

/* 文本正则化（等价于 post_tn(WeText(pre_tn(text)))），输出 UTF-8 */
int lingtts_fe_normalize(lingtts_fe_t *fe, const char *text_utf8, char *out, size_t out_size);

/* 任意长度文本：切句 → 正则化 → G2P → 按 max_tokens（与合成器的 cfg.max_tokens 相同）切段，每段回调一次。
 * 回调返回非 0 时停止并返回该值 */
typedef int (*lingtts_fe_chunk_cb)(const lingtts_tokens_t *tok, void *user);
int lingtts_fe_text(lingtts_fe_t *fe, const char *text, uint16_t max_tokens, lingtts_fe_chunk_cb cb, void *user);

/* 文本 → PCM：lingtts_fe_text + lingtts_synth_tokens */
int lingtts_fe_synth(lingtts_fe_t *fe, lingtts_t *tts, uint16_t max_tokens, const char *text, lingtts_pcm_cb cb,
                     void *user);

/* 调试：纯汉字串的分词结果（等价于 jieba.lcut），"/" 分隔 */
int lingtts_fe_segment(lingtts_fe_t *fe, const char *han_utf8, char *out, size_t size);

/* 音节 id → 拼音字符串（如 "zhong1"） */
const char *lingtts_fe_syl_name(const lingtts_fe_t *fe, uint16_t syl, char buf[12]);

#ifdef __cplusplus
}
#endif
