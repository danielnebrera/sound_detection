/* =================================================================
 * audio_recorder.c — transmisión BINARIA con CRC32
 *
 * Protocolo por canal:
 *   [CHx_BIN]\r\n              ← header ASCII (10 bytes)
 *   <88200 bytes int16 LE>     ← payload binario (44100 × 2)
 *   <4 bytes CRC32 LE>         ← checksum del payload
 *   [CHx_END] sent=44100 errors=0\r\n  ← confirmación ASCII
 *
 * Flujo completo:
 *   [CHUNK_START] 44100\r\n
 *   [CH0_BIN]\r\n <payload+CRC>  [CH0_END] sent=44100 errors=0\r\n
 *   [CH1_BIN]\r\n <payload+CRC>  [CH1_END] ...
 *   [CH2_BIN]\r\n <payload+CRC>  [CH2_END] ...
 *   [CH3_BIN]\r\n <payload+CRC>  [CH3_END] ...
 *   [CHUNK_END]\r\n
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

/* ── CRC32 (polinomio IEEE 802.3) ────────────────────────────────── */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

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

static bool check_stop(void)
{
    uint8_t c = 0;
    if (HAL_UART_Receive(&huart1, &c, 1, 0) == HAL_OK)
        if (c == 'S') s_stop = true;
    return s_stop;
}

/* Emite un canal completo en binario con CRC32 */
static void emit_channel_binary(int ch)
{
    char hdr[32];
    const char *names[] = {"CH0","CH1","CH2","CH3"};

    /* Header ASCII */
    snprintf(hdr, sizeof(hdr), "[%s_BIN]\r\n", names[ch]);
    uart_puts(hdr);

    /* Payload: 44100 muestras int16 little-endian en bloques de 512 */
    #define TX_BLOCK 512
    uint8_t  tx_buf[TX_BLOCK * 2];
    uint32_t crc    = 0;
    uint32_t sent   = 0;
    uint32_t errors = 0;
    uint32_t i      = 0;

    while (i < RECORD_FRAMES && !s_stop)
    {
        uint32_t block = RECORD_FRAMES - i;
        if (block > TX_BLOCK) block = TX_BLOCK;

        /* Empaquetar int16 LE */
        for (uint32_t j = 0; j < block; j++) {
            int16_t v = s_chunk.samples[i + j][ch];
            tx_buf[j * 2]     = (uint8_t)(v & 0xFF);
            tx_buf[j * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
        }

        uint32_t bytes = block * 2;
        crc = crc32_update(crc, tx_buf, bytes);

        HAL_StatusTypeDef st =
            HAL_UART_Transmit(&huart1, tx_buf, bytes, HAL_MAX_DELAY);

        if (st == HAL_OK) sent   += block;
        else              errors += block;

        i += block;

        if (i % 10000 == 0) check_stop();
    }

    /* CRC32 little-endian (4 bytes) */
    uint8_t crc_buf[4] = {
        (uint8_t)(crc & 0xFF),
        (uint8_t)((crc >> 8)  & 0xFF),
        (uint8_t)((crc >> 16) & 0xFF),
        (uint8_t)((crc >> 24) & 0xFF)
    };
    HAL_UART_Transmit(&huart1, crc_buf, 4, HAL_MAX_DELAY);

    /* Confirmación ASCII */
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
        emit_channel_binary(ch);

    uart_puts("[CHUNK_END]\r\n");

    memset(&s_chunk, 0, sizeof(s_chunk));
    s_write_frame = 0;
    s_chunk_ready = false;

    return !s_stop;
}

bool audio_recorder_stop_requested(void)
{
    return s_stop;
}
