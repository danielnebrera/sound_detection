/* =================================================================
 * audio_recorder.c
 *
 * Captura 4 canales simultáneos durante exactamente RECORD_FRAMES.
 * Buffer único intercalado en RAM D1.
 * ================================================================= */

#include "audio_recorder.h"
#include "audio_capture.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;
extern AudioCaptureContext g_audio_ctx;

/* ── Buffer principal en RAM D1 — 352,800 bytes ──────────────────── */
static RecorderChunk s_chunk;

static uint32_t s_write_frame = 0;
static bool     s_chunk_ready = false;
static bool     s_stop        = false;

/* ── Helpers ─────────────────────────────────────────────────────── */

static inline int16_t pcm24_to_pcm16(int32_t value)
{
    int32_t s = value >> 8;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static void uart_puts(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)s, strlen(s), HAL_MAX_DELAY);
}

static void emit_hex(int32_t val)
{
    char buf[11];
    uint32_t w = (uint32_t)val;
    static const char h[] = "0123456789ABCDEF";
    buf[0]=h[(w>>28)&0xF]; buf[1]=h[(w>>24)&0xF];
    buf[2]=h[(w>>20)&0xF]; buf[3]=h[(w>>16)&0xF];
    buf[4]=h[(w>>12)&0xF]; buf[5]=h[(w>> 8)&0xF];
    buf[6]=h[(w>> 4)&0xF]; buf[7]=h[(w>> 0)&0xF];
    buf[8]='\r'; buf[9]='\n'; buf[10]='\0';
    HAL_UART_Transmit(&huart1, (uint8_t*)buf, 10, HAL_MAX_DELAY);
}

static HAL_StatusTypeDef emit_hex_status(int32_t val)
{
    char buf[11];
    uint32_t w = (uint32_t)val;
    static const char h[] = "0123456789ABCDEF";
    buf[0]=h[(w>>28)&0xF]; buf[1]=h[(w>>24)&0xF];
    buf[2]=h[(w>>20)&0xF]; buf[3]=h[(w>>16)&0xF];
    buf[4]=h[(w>>12)&0xF]; buf[5]=h[(w>> 8)&0xF];
    buf[6]=h[(w>> 4)&0xF]; buf[7]=h[(w>> 0)&0xF];
    buf[8]='\r'; buf[9]='\n'; buf[10]='\0';
    return HAL_UART_Transmit(&huart1, (uint8_t*)buf, 10, HAL_MAX_DELAY);
}

static bool check_stop(void)
{
    uint8_t c = 0;
    if (HAL_UART_Receive(&huart1, &c, 1, 0) == HAL_OK)
        if (c == 'S') s_stop = true;
    return s_stop;
}

static void emit_channel(int ch)
{
    char hdr[48];
    const char *names[] = {"CH0","CH1","CH2","CH3"};

    snprintf(hdr, sizeof(hdr), "[%s_START]\r\n", names[ch]);
    uart_puts(hdr);

    uint32_t sent   = 0;
    uint32_t errors = 0;

    for (uint32_t i = 0; i < RECORD_FRAMES; i++) {
        int32_t val = ((int32_t)s_chunk.samples[i][ch]) << 8;
        HAL_StatusTypeDef st = emit_hex_status(val);
        if (st == HAL_OK) sent++;
        else              errors++;
        if (i % 10000 == 0) check_stop();
    }

    snprintf(hdr, sizeof(hdr), "[%s_END] sent=%lu errors=%lu\r\n",
             names[ch], (unsigned long)sent, (unsigned long)errors);
    uart_puts(hdr);
}

/* ── API pública ─────────────────────────────────────────────────── */

void audio_recorder_init(void)
{
    memset(&s_chunk, 0, sizeof(s_chunk));
    s_write_frame = 0;
    s_chunk_ready = false;
    s_stop        = false;
    uart_puts("[RECORD_READY]\r\n");
}

void audio_recorder_accumulate(void)
{
    if (s_chunk_ready) return;

    int32_t *ch0 = audio_capture_get_channel(&g_audio_ctx, 0);
    int32_t *ch1 = audio_capture_get_channel(&g_audio_ctx, 1);
    int32_t *ch2 = audio_capture_get_channel(&g_audio_ctx, 2);
    int32_t *ch3 = audio_capture_get_channel(&g_audio_ctx, 3);

    if (!ch0 || !ch1 || !ch2 || !ch3) return;

    uint32_t src_offset = 0;

    while (src_offset < AUDIO_BUFFER_SIZE)
    {
        uint32_t src_rem   = AUDIO_BUFFER_SIZE - src_offset;
        uint32_t chunk_rem = RECORD_FRAMES    - s_write_frame;
        uint32_t count     = src_rem < chunk_rem ? src_rem : chunk_rem;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t src = src_offset + i;
            uint32_t dst = s_write_frame + i;
            s_chunk.samples[dst][0] = pcm24_to_pcm16(ch0[src]);
            s_chunk.samples[dst][1] = pcm24_to_pcm16(ch1[src]);
            s_chunk.samples[dst][2] = pcm24_to_pcm16(ch2[src]);
            s_chunk.samples[dst][3] = pcm24_to_pcm16(ch3[src]);
        }

        src_offset    += count;
        s_write_frame += count;

        if (s_write_frame == RECORD_FRAMES) {
            s_chunk_ready = true;
            break;
        }
    }
}

bool audio_recorder_is_ready(void)
{
    return s_chunk_ready;
}

bool audio_recorder_emit_and_reset(void)
{
    if (!s_chunk_ready) return true;

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[CHUNK_START] %lu\r\n",
             (unsigned long)RECORD_FRAMES);
    uart_puts(hdr);

    for (int ch = 0; ch < (int)RECORD_CHANNELS && !s_stop; ch++)
        emit_channel(ch);

    uart_puts("[CHUNK_END]\r\n");

    /* Reiniciar para el siguiente chunk */
    memset(&s_chunk, 0, sizeof(s_chunk));
    s_write_frame = 0;
    s_chunk_ready = false;

    return !s_stop;
}

bool audio_recorder_stop_requested(void)
{
    return s_stop;
}
