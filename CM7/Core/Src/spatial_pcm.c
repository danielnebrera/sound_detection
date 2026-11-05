#include "spatial_pcm.h"
#include "portenta_sdram.h"
#include "usart.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

#define SPCM_FRAMES 4410U
#define SPCM_START ((RECORD_FRAMES - SPCM_FRAMES)/2U)
#define SPCM_BYTES (SPCM_FRAMES*RECORD_CHANNELS*2U)
#define SPCM_RING_BYTES (AUDIO_RING_SLOT_COUNT*RECORD_CHANNELS*RECORD_FRAMES*2U)
/* Reserve SDRAM immediately AFTER the recorder ring. Not a linker allocation. */
#define SPCM_ADDRESS (PORTENTA_SDRAM_BASE_ADDRESS+SPCM_RING_BYTES)
#if (SPCM_RING_BYTES + SPCM_BYTES > PORTENTA_SDRAM_SIZE_BYTES)
#error "Spatial PCM snapshot exceeds SDRAM"
#endif
#if (RECORD_CHANNELS != 4U) || (RECORD_FRAMES != 44100U)
#error "Spatial PCM layout mismatch"
#endif

extern UART_HandleTypeDef huart1;
/* P3.4: 12 diagnostic snapshots, separate from the recorder ring. */
#define SPCM_COUNT 12U
static const uint32_t s_orders[SPCM_COUNT] = {3U,5U,7U,9U,10U,11U,13U,15U,17U,19U,21U,23U};
static uint8_t s_taken[SPCM_COUNT];
static uint32_t s_crc[SPCM_COUNT];
static int16_t (*const s_pcm)[RECORD_CHANNELS][SPCM_FRAMES] =
    (int16_t (*)[RECORD_CHANNELS][SPCM_FRAMES])SPCM_ADDRESS;
#if (SPCM_RING_BYTES + SPCM_BYTES * SPCM_COUNT > PORTENTA_SDRAM_SIZE_BYTES)
#error "P3.4 snapshots exceed SDRAM"
#endif
/* SDRAM COHERENT SNAPSHOT: exactly ONE reusable 35,280-byte buffer.
 * Standard .bss: the linker MUST confirm sufficient internal RAM.
 * Never placed in the recorder SDRAM ring or tensor arena explicitly.
 * Verify its address in the final .map before flashing. */
#if defined(__GNUC__)
__attribute__((aligned(32), used))
#endif
static uint8_t s_spcm_frozen[SPCM_BYTES];

#define SPCM_COPY_PASSES 4U
/* MEMORY DIFFERENCE + ACCESS WIDTH DIAG; no new PCM window. */
#define SPCM_DIFF_SAMPLES 16U
#define SPCM_WIDTH_READS 8U
#define SPCM_KNOWN_OFFSET 970U
#define SPCM_PERSIST_READS 24U
#define SPCM_PERSIST_POINTS 4U /* historical offset + first three differences */
typedef struct {
    uint32_t offset;
    uint8_t ram;
    uint8_t sdram_first;
    uint8_t sdram_second;
    uint8_t ram_pair_lo;
    uint8_t ram_pair_hi;
    uint8_t src_pair_lo;
    uint8_t src_pair_hi;
    uint8_t width8_first, width16_first, width32_first;
    uint8_t width8_last, width16_last, width32_last;
    uint8_t width8_changes, width16_changes, width32_changes;
    uint8_t width16_disagree, width32_disagree;
} SpcmDifference;


/* ACCESS WIDTH DIAG: direct, volatile and aligned SDRAM reads. Never writes.
 * A 16/32-bit access is aligned DOWN to its natural boundary, then the byte
 * corresponding to offset is extracted with the little-endian byte shift.
 * The 8/16/32-bit reads happen in sequence, so any difference across widths
 * is an observation, NOT proof that access width caused it. */
static void spcm_probe_widths(const uint8_t *base, uint32_t offset,
                              SpcmDifference *d)
{
    const uintptr_t addr = (uintptr_t)(base + offset);
    const volatile uint8_t *p8 = (const volatile uint8_t *)addr;
    const volatile uint16_t *p16 =
        (const volatile uint16_t *)(addr & ~(uintptr_t)1U);
    const volatile uint32_t *p32 =
        (const volatile uint32_t *)(addr & ~(uintptr_t)3U);
    const uint32_t shift16 = (offset & 1U) * 8U;
    const uint32_t shift32 = (offset & 3U) * 8U;
    d->width8_changes = d->width16_changes = d->width32_changes = 0U;
    d->width16_disagree = d->width32_disagree = 0U;
    d->width8_first = d->width16_first = d->width32_first = 0U;
    d->width8_last = d->width16_last = d->width32_last = 0U;
    for (uint32_t t = 0U; t < SPCM_WIDTH_READS; ++t) {
        const uint8_t b8 = *p8;
        const uint8_t b16 = (uint8_t)((*p16 >> shift16) & 0xFFU);
        const uint8_t b32 = (uint8_t)((*p32 >> shift32) & 0xFFU);
        if (t == 0U) {
            d->width8_first = b8;
            d->width16_first = b16;
            d->width32_first = b32;
        } else {
            d->width8_changes += (uint8_t)(b8 != d->width8_last);
            d->width16_changes += (uint8_t)(b16 != d->width16_last);
            d->width32_changes += (uint8_t)(b32 != d->width32_last);
        }
        d->width16_disagree += (uint8_t)(b8 != b16);
        d->width32_disagree += (uint8_t)(b8 != b32);
        d->width8_last = b8;
        d->width16_last = b16;
        d->width32_last = b32;
    }
}


/* READ-ONLY PERSISTENCE: 24 consecutive volatile byte reads, plus
 * aligned 16/32-bit reads, at a single PCM offset. No SDRAM writes.
 * This is done after STOP, while the first frozen RAM copy is untouched.
 * Record a compact timeline: bit-6 status for each round. This reveals
 * 0->1->0 transitions that first/last-only logs would hide. */
typedef struct {
    uint32_t offset;
    uint8_t ram;
    uint8_t first8, last8;
    uint8_t changes8, changes16, changes32;
    uint8_t mismatch16, mismatch32;
    uint8_t count0, count1;
    uint32_t bit6_seq8;
    uint32_t bit6_seq16;
    uint32_t bit6_seq32;
} SpcmPersist;

static void spcm_probe_persistence(const uint8_t *base, uint32_t off,
                                   uint8_t ram, SpcmPersist *out)
{
    const uintptr_t addr = (uintptr_t)(base + off);
    const volatile uint8_t *p8 = (const volatile uint8_t *)addr;
    const volatile uint16_t *p16 = (const volatile uint16_t *)(addr & ~(uintptr_t)1U);
    const volatile uint32_t *p32 = (const volatile uint32_t *)(addr & ~(uintptr_t)3U);
    const uint32_t sh16 = (off & 1U) * 8U;
    const uint32_t sh32 = (off & 3U) * 8U;
    memset(out, 0, sizeof(*out));
    out->offset = off;
    out->ram = ram;
    uint8_t prev8 = 0U, prev16 = 0U, prev32 = 0U;
    for (uint32_t n = 0U; n < SPCM_PERSIST_READS; ++n) {
        const uint8_t a = *p8;
        const uint8_t b = (uint8_t)((*p16 >> sh16) & 0xFFU);
        const uint8_t c = (uint8_t)((*p32 >> sh32) & 0xFFU);
        if (n == 0U) out->first8 = a;
        else {
            out->changes8 += (uint8_t)(a != prev8);
            out->changes16 += (uint8_t)(b != prev16);
            out->changes32 += (uint8_t)(c != prev32);
        }
        out->bit6_seq8 |= (uint32_t)(((a >> 6U) & 1U) << n);
        out->bit6_seq16 |= (uint32_t)(((b >> 6U) & 1U) << n);
        out->bit6_seq32 |= (uint32_t)(((c >> 6U) & 1U) << n);
        out->mismatch16 += (uint8_t)(a != b);
        out->mismatch32 += (uint8_t)(a != c);
        out->count0 += (uint8_t)((a & 0x40U) == 0U);
        out->count1 += (uint8_t)((a & 0x40U) != 0U);
        prev8 = a; prev16 = b; prev32 = c;
        out->last8 = a;
    }
}


/* FULL SCAN CRC DIAG: complete source traversal using explicit aligned
 * volatile 16-bit loads. CRC byte order is PCM16 little-endian.
 * The SRAM reference is NOT modified by this function.
 * Results describe access observations, not proof of physical SDRAM damage. */
typedef struct {
    uint32_t memcpy_crc;
    uint32_t v16_crc_first;
    uint32_t v16_crc_second;
    uint32_t different_bytes;
    uint32_t only_bit6;
    uint32_t other_bits;
    uint32_t first_offset;
    uint32_t first_xor;
} SpcmFullScan;

static uint32_t spcm_crc16_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint32_t bit = 0U; bit < 8U; ++bit)
        crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    return crc;
}

static uint32_t spcm_scan_volatile16(const uint8_t *source,
                                      const uint8_t *reference,
                                      SpcmFullScan *result,
                                      bool record_differences)
{
    /* Both input bases are naturally aligned; SPCM_BYTES is even.
     * Only reads. No additional copy or DMA is involved. */
    const volatile uint16_t *p = (const volatile uint16_t *)source;
    uint32_t state = 0xFFFFFFFFU;
    for (uint32_t sample = 0U; sample < SPCM_BYTES / 2U; ++sample) {
        const uint16_t value = p[sample];
        const uint8_t lo = (uint8_t)value;
        const uint8_t hi = (uint8_t)(value >> 8U);
        state = spcm_crc16_byte(state, lo);
        state = spcm_crc16_byte(state, hi);
        if (record_differences) {
            const uint32_t offset = sample * 2U;
            const uint8_t xor_lo = (uint8_t)(lo ^ reference[offset]);
            const uint8_t xor_hi = (uint8_t)(hi ^ reference[offset + 1U]);
            if (xor_lo != 0U) {
                ++result->different_bytes;
                if (xor_lo == 0x40U) ++result->only_bit6;
                else ++result->other_bits;
                if (result->first_offset == 0xFFFFFFFFU) {
                    result->first_offset = offset;
                    result->first_xor = xor_lo;
                }
            }
            if (xor_hi != 0U) {
                ++result->different_bytes;
                if (xor_hi == 0x40U) ++result->only_bit6;
                else ++result->other_bits;
                if (result->first_offset == 0xFFFFFFFFU) {
                    result->first_offset = offset + 1U;
                    result->first_xor = xor_hi;
                }
            }
        }
    }
    return ~state;
}

static uint32_t crc32_pcm(const uint8_t *data, uint32_t n)
{
    uint32_t crc=0xFFFFFFFFU;
    for(uint32_t i=0;i<n;i++) {
        crc ^= data[i];
        for(uint32_t b=0;b<8U;b++) crc=(crc>>1U)^((crc&1U)?0xEDB88320U:0U);
    }
    return ~crc;
}
/* P3.4 CRC SOURCE DIAG: incremental CRC over each UNIQUE wire payload. */
static uint32_t crc32_update_state(uint32_t state, const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0U; i < n; ++i) {
        state ^= data[i];
        for (uint32_t b = 0U; b < 8U; ++b)
            state = (state >> 1U) ^ ((state & 1U) ? 0xEDB88320U : 0U);
    }
    return state;
}

void spatial_pcm_reset(void) { memset(s_taken,0,sizeof(s_taken)); memset(s_crc,0,sizeof(s_crc)); }
bool spatial_pcm_capture(uint32_t record_order,const RecorderChunkView *view)
{
    if(!view || view->frame_count!=RECORD_FRAMES) return false;
    for(uint32_t ch=0;ch<RECORD_CHANNELS;ch++) if(!view->channel[ch]) return false;
    for(uint32_t k=0;k<SPCM_COUNT;k++) {
        if(s_orders[k]!=record_order || s_taken[k]) continue;
        for(uint32_t ch=0;ch<RECORD_CHANNELS;ch++)
            memcpy(s_pcm[k][ch],view->channel[ch]+SPCM_START,SPCM_FRAMES*sizeof(int16_t));
        __DMB();
        /* CRC is calculated after STOP, not on the live acquisition path. */
        s_taken[k]=1U;
        return true;
    }
    return false;
}
bool spatial_pcm_has_capture(void)
{
    for(uint32_t k=0;k<SPCM_COUNT;k++) if(s_taken[k]) return true;
    return false;
}
/* SPATIAL-P2.4: indexed ACK / NACK and bounded block retries.
 * Header is text; host sends G to begin. Every packet: 'SPB2', uint16 LE index,
 * uint16 LE length, uint32 LE crc32, payload; host responds A/N + uint16 index.
 * No UART is added to the sampling/ISR path. */
#define SPCM_BLOCK_SIZE 1024U
#define SPCM_MAX_RETRIES 12U
static bool send(const uint8_t *p, uint16_t n)
{
    return HAL_UART_Transmit(&huart1, (uint8_t*)p, n, 3000U) == HAL_OK;
}
/* The response is indexed. Stale ACKs cannot validate a different block. */
static bool wait_indexed_ack(uint16_t target, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t kind = 0U;
    uint8_t bytes[2];
    while ((uint32_t)(HAL_GetTick()-start) < timeout_ms) {
        if (HAL_UART_Receive(&huart1, &kind, 1U, 25U) != HAL_OK) continue;
        if (kind != (uint8_t)'A' && kind != (uint8_t)'N') continue;
        if (HAL_UART_Receive(&huart1, bytes, 2U, 200U) != HAL_OK) continue;
        uint16_t index = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
        if (index == target) return kind == (uint8_t)'A';
    }
    return false;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U); p[3] = (uint8_t)(v >> 24U);
}
bool spatial_pcm_transmit_after_stop(void)
{
    char line[240];
    if (!spatial_pcm_has_capture()) {
        const char *msg = "[SPCM_NONE] captured=0\r\n";
        return send((const uint8_t*)msg, (uint16_t)strlen(msg));
    }
    uint32_t sent=0U;
    for(uint32_t k=0;k<SPCM_COUNT;k++) {
        if(!s_taken[k]) continue;
        const uint8_t *pcm=(const uint8_t*)s_pcm[k];
        /* COPY ISOLATION: executed ONLY after STOP. No recorder/MPU changes.
         * RAM_A/RAM_B: same frozen buffer, no write between CRC passes.
         * SRC_COMPARE: direct bytewise SDRAM reread against frozen RAM copy,
         * before any subsequent memcpy; counts differences and first offset.
         * COPY_NEXT: another SDRAM->RAM copy, followed by two RAM CRCs.
         * We require both RAM CRC agreement AND two consecutive copy CRCs
         * to accept a frozen snapshot. No additional full-window buffer. */
        uint32_t memory_crc1 = 0U;
        uint32_t memory_crc2 = 0U;
        uint32_t last_crc = 0U;
        uint32_t copy_passes = 0U;
        uint32_t ram_mismatch_passes = 0U;
        uint32_t ram_first_a = 0U;
        uint32_t ram_first_b = 0U;
        uint32_t src_diff_first = 0xFFFFFFFFU;
        uint32_t src_diff_count = 0U;
        uint8_t src_first_value = 0U;
        uint8_t ram_first_value = 0U;
        bool stable_copy = false;
        /* Measurements are local to this snapshot and occur after STOP. */
        SpcmDifference differences[SPCM_DIFF_SAMPLES];
        uint32_t diff_shown = 0U;
        uint32_t diff_by_channel[RECORD_CHANNELS] = {0U};
        uint32_t diff_src_changed = 0U;
        SpcmDifference known_probe = {0};
        SpcmPersist persist[SPCM_PERSIST_POINTS];
        uint32_t persist_count = 0U;
        bool known_probe_valid = (SPCM_KNOWN_OFFSET < SPCM_BYTES);
        SpcmFullScan scans[SPCM_COPY_PASSES] = {0};
        for (uint32_t pass = 0U; pass < SPCM_COPY_PASSES; ++pass) {
            memcpy(s_spcm_frozen, pcm, SPCM_BYTES);
            __DMB();
            const uint32_t ram_a = crc32_pcm(s_spcm_frozen, SPCM_BYTES);
            __asm__ volatile ("" ::: "memory"); /* force independent read pass */
            const uint32_t ram_b = crc32_pcm(s_spcm_frozen, SPCM_BYTES);
            if (pass == 0U) { ram_first_a = ram_a; ram_first_b = ram_b; }
            if (pass == 0U) memory_crc1 = ram_a;
            if (pass == 1U) memory_crc2 = ram_a;
            copy_passes = pass + 1U;
            if (ram_a != ram_b) ram_mismatch_passes++;
            /* The memcpy reference remains frozen for BOTH direct scans.
             * Two full volatile16 reads make transient source differences
             * visible independently of the next memcpy attempt. */
            SpcmFullScan *scan = &scans[pass];
            scan->memcpy_crc = ram_a;
            scan->first_offset = 0xFFFFFFFFU;
            __DMB();
            scan->v16_crc_first = spcm_scan_volatile16(pcm, s_spcm_frozen,
                                                      scan, true);
            __DMB();
            scan->v16_crc_second = spcm_scan_volatile16(pcm, s_spcm_frozen,
                                                       scan, false);


            /* Revisit source one byte at a time while frozen copy is intact.
             * This identifies a difference, not which side is physically bad.
             * Collect on FIRST pass only to limit additional SDRAM traffic. */
            if (pass == 0U) {
                const volatile uint8_t *src_live =
                    (const volatile uint8_t *)pcm;
                for (uint32_t i = 0U; i < SPCM_BYTES; ++i) {
                    const uint8_t a = src_live[i];
                    const uint8_t b = s_spcm_frozen[i];
                    if (a != b) {
                        if (src_diff_first == 0xFFFFFFFFU) {
                            src_diff_first = i;
                            src_first_value = a;
                            ram_first_value = b;
                        }
                        ++src_diff_count;
                        const uint32_t ch = i / (SPCM_FRAMES * 2U);
                        if (ch < RECORD_CHANNELS) ++diff_by_channel[ch];
                        if (diff_shown < SPCM_DIFF_SAMPLES) {
                            /* Read the same SDRAM byte again immediately.
                             * These are observations, not a proof of cause. */
                            const uint8_t a2 = src_live[i];
                            if (a2 != a) ++diff_src_changed;
                            const uint32_t pair = i & ~1U;
                            SpcmDifference *d = &differences[diff_shown++];
                            d->offset = i;
                            d->ram = b;
                            d->sdram_first = a;
                            d->sdram_second = a2;
                            d->ram_pair_lo = s_spcm_frozen[pair];
                            d->ram_pair_hi = s_spcm_frozen[pair + 1U];
                            d->src_pair_lo = src_live[pair];
                            d->src_pair_hi = src_live[pair + 1U];
                            spcm_probe_widths(pcm, i, d);
                            if (persist_count < SPCM_PERSIST_POINTS - 1U) {
                                spcm_probe_persistence(pcm, i, b,
                                                       &persist[persist_count++]);
                            }
                        }
                    }
                }
                /* Fixed historical offset even if this run has zero diffs.
                 * Captured during first pass while RAM reference is intact. */
                if (known_probe_valid) {
                    known_probe.offset = SPCM_KNOWN_OFFSET;
                    known_probe.ram = s_spcm_frozen[SPCM_KNOWN_OFFSET];
                    spcm_probe_widths(pcm, SPCM_KNOWN_OFFSET, &known_probe);
                    spcm_probe_persistence(pcm, SPCM_KNOWN_OFFSET,
                                           known_probe.ram,
                                           &persist[SPCM_PERSIST_POINTS - 1U]);
                }
            }
            if (pass > 0U && ram_a == ram_b && ram_a == last_crc) {
                stable_copy = true;
                break;
            }
            last_crc = ram_a;
        }
        /* FULL SCAN reports are emitted only after all measurements finish.
         * Existing monitor ignores these new diagnostic prefixes. */
        uint32_t scan_mismatch_rounds = 0U;
        uint32_t scan_v16_unstable_rounds = 0U;
        uint32_t scan_bit6_total = 0U;
        uint32_t scan_other_total = 0U;
        for (uint32_t si = 0U; si < copy_passes; ++si) {
            const SpcmFullScan *q = &scans[si];
            scan_mismatch_rounds += (uint32_t)(q->memcpy_crc != q->v16_crc_first);
            scan_v16_unstable_rounds +=
                (uint32_t)(q->v16_crc_first != q->v16_crc_second);
            scan_bit6_total += q->only_bit6;
            scan_other_total += q->other_bits;
            int sn = snprintf(line, sizeof(line),
                "[SPCM_SCAN] seq=%lu round=%lu memcpy=%08lX v16a=%08lX "
                "v16b=%08lX match=%u v16_stable=%u diff=%lu "
                "xor40=%lu other=%lu first=%ld first_xor=%02lX\r\n",
                (unsigned long)s_orders[k], (unsigned long)(si + 1U),
                (unsigned long)q->memcpy_crc,
                (unsigned long)q->v16_crc_first,
                (unsigned long)q->v16_crc_second,
                (unsigned)(q->memcpy_crc == q->v16_crc_first),
                (unsigned)(q->v16_crc_first == q->v16_crc_second),
                (unsigned long)q->different_bytes,
                (unsigned long)q->only_bit6,
                (unsigned long)q->other_bits,
                (long)(q->first_offset == 0xFFFFFFFFU ? -1L : (long)q->first_offset),
                (unsigned long)q->first_xor);
            if (sn <= 0 || (size_t)sn >= sizeof(line) ||
                !send((const uint8_t *)line, (uint16_t)sn)) return false;
        }
        int ss = snprintf(line, sizeof(line),
            "[SPCM_SCAN_SUMMARY] seq=%lu rounds=%lu mismatch=%lu "
            "v16_unstable=%lu xor40=%lu other=%lu\r\n",
            (unsigned long)s_orders[k], (unsigned long)copy_passes,
            (unsigned long)scan_mismatch_rounds,
            (unsigned long)scan_v16_unstable_rounds,
            (unsigned long)scan_bit6_total,
            (unsigned long)scan_other_total);
        if (ss <= 0 || (size_t)ss >= sizeof(line) ||
            !send((const uint8_t *)line, (uint16_t)ss)) return false;

        /* MEMORY DIFFERENCE DIAG: print only AFTER the copying loop.
         * No verbose UART while reading SDRAM or during live capture.
         * Offset is in planar PCM16 bytes, relative to this snapshot. */
        if (src_diff_count != 0U) {
            for (uint32_t d_idx = 0U; d_idx < diff_shown; ++d_idx) {
                const SpcmDifference *d = &differences[d_idx];
                const uint32_t channel = d->offset / (SPCM_FRAMES * 2U);
                const uint32_t sample = (d->offset % (SPCM_FRAMES * 2U)) / 2U;
                const uint32_t high_byte = d->offset & 1U;
                const uint16_t ram_word = (uint16_t)(
                    (uint16_t)d->ram_pair_lo | ((uint16_t)d->ram_pair_hi << 8U));
                const uint16_t src_word = (uint16_t)(
                    (uint16_t)d->src_pair_lo | ((uint16_t)d->src_pair_hi << 8U));
                int dlen = snprintf(line, sizeof(line),
                    "[SPCM_DIFF] seq=%lu off=%lu ch=%lu sample=%lu part=%s "
                    "ram=%02X src1=%02X src2=%02X ram16=%04X src16=%04X\r\n",
                    (unsigned long)s_orders[k], (unsigned long)d->offset,
                    (unsigned long)channel, (unsigned long)sample,
                    high_byte ? "HIGH" : "LOW", (unsigned)d->ram,
                    (unsigned)d->sdram_first, (unsigned)d->sdram_second,
                    (unsigned)ram_word, (unsigned)src_word);
                if (dlen <= 0 || (size_t)dlen >= sizeof(line) ||
                    !send((const uint8_t *)line, (uint16_t)dlen)) return false;
            }
        }
        /* Width observations are emitted AFTER copying, exclusively after STOP.
         * Results are from volatile aligned 8/16/32-bit accesses to SDRAM. */
        if (known_probe_valid) {
            const SpcmDifference *d = &known_probe;
            int w = snprintf(line, sizeof(line),
                "[SPCM_WIDTH] seq=%lu kind=KNOWN off=%lu ram=%02X "
                "b8=%02X/%02X b16=%02X/%02X b32=%02X/%02X "
                "chg=%u/%u/%u disagree=%u/%u n=%u\r\n",
                (unsigned long)s_orders[k], (unsigned long)d->offset,
                (unsigned)d->ram,
                (unsigned)d->width8_first, (unsigned)d->width8_last,
                (unsigned)d->width16_first, (unsigned)d->width16_last,
                (unsigned)d->width32_first, (unsigned)d->width32_last,
                (unsigned)d->width8_changes, (unsigned)d->width16_changes,
                (unsigned)d->width32_changes,
                (unsigned)d->width16_disagree, (unsigned)d->width32_disagree,
                (unsigned)SPCM_WIDTH_READS);
            if (w <= 0 || (size_t)w >= sizeof(line) ||
                !send((const uint8_t *)line, (uint16_t)w)) return false;
        }
        for (uint32_t d_idx = 0U; d_idx < diff_shown; ++d_idx) {
            const SpcmDifference *d = &differences[d_idx];
            int w = snprintf(line, sizeof(line),
                "[SPCM_WIDTH] seq=%lu kind=DIFF off=%lu ram=%02X "
                "b8=%02X/%02X b16=%02X/%02X b32=%02X/%02X "
                "chg=%u/%u/%u disagree=%u/%u n=%u\r\n",
                (unsigned long)s_orders[k], (unsigned long)d->offset,
                (unsigned)d->ram,
                (unsigned)d->width8_first, (unsigned)d->width8_last,
                (unsigned)d->width16_first, (unsigned)d->width16_last,
                (unsigned)d->width32_first, (unsigned)d->width32_last,
                (unsigned)d->width8_changes, (unsigned)d->width16_changes,
                (unsigned)d->width32_changes,
                (unsigned)d->width16_disagree, (unsigned)d->width32_disagree,
                (unsigned)SPCM_WIDTH_READS);
            if (w <= 0 || (size_t)w >= sizeof(line) ||
                !send((const uint8_t *)line, (uint16_t)w)) return false;
        }
        /* Persistence snapshots were measured BEFORE repeated copies and
         * are printed ONLY after all source reads have completed. */
        for (uint32_t pi = 0U; pi < persist_count + (known_probe_valid ? 1U : 0U); ++pi) {
            const bool historical = (known_probe_valid && pi == persist_count);
            const SpcmPersist *q = historical ?
                &persist[SPCM_PERSIST_POINTS - 1U] : &persist[pi];
            int w = snprintf(line, sizeof(line),
                "[SPCM_PERSIST] seq=%lu kind=%s off=%lu ram=%02X "
                "first=%02X last=%02X changes=%u/%u/%u "
                "width_diff=%u/%u bit6_zero=%u bit6_one=%u "
                "mask8=%06lX mask16=%06lX mask32=%06lX n=%u\r\n",
                (unsigned long)s_orders[k], historical ? "KNOWN" : "DIFF",
                (unsigned long)q->offset, (unsigned)q->ram,
                (unsigned)q->first8, (unsigned)q->last8,
                (unsigned)q->changes8, (unsigned)q->changes16,
                (unsigned)q->changes32, (unsigned)q->mismatch16,
                (unsigned)q->mismatch32, (unsigned)q->count0,
                (unsigned)q->count1, (unsigned long)q->bit6_seq8,
                (unsigned long)q->bit6_seq16,
                (unsigned long)q->bit6_seq32,
                (unsigned)SPCM_PERSIST_READS);
            if (w <= 0 || (size_t)w >= sizeof(line) ||
                !send((const uint8_t *)line, (uint16_t)w)) return false;
        }
        int dsummary = snprintf(line, sizeof(line),
            "[SPCM_DIFF_SUMMARY] seq=%lu total=%lu shown=%lu "
            "ch0=%lu ch1=%lu ch2=%lu ch3=%lu src1_src2_changed=%lu\r\n",
            (unsigned long)s_orders[k], (unsigned long)src_diff_count,
            (unsigned long)diff_shown,
            (unsigned long)diff_by_channel[0], (unsigned long)diff_by_channel[1],
            (unsigned long)diff_by_channel[2], (unsigned long)diff_by_channel[3],
            (unsigned long)diff_src_changed);
        if (dsummary <= 0 || (size_t)dsummary >= sizeof(line) ||
            !send((const uint8_t *)line, (uint16_t)dsummary)) return false;
        /* Always report what was measured before attempting the UART batch.
         * Diagnostics are after STOP, never on the live recording path. */
        int iso_n = snprintf(line, sizeof(line),
            "[SPCM_COPY_ISOLATION] seq=%lu passes=%lu "
            "copy1=%08lX copy2=%08lX last=%08lX "
            "ram1=%08lX ram2=%08lX ram_mismatch=%lu "
            "src_diff=%lu first_offset=%ld "
            "src_byte=%02X ram_byte=%02X stable=%u\r\n",
            (unsigned long)s_orders[k], (unsigned long)copy_passes,
            (unsigned long)memory_crc1, (unsigned long)memory_crc2,
            (unsigned long)last_crc, (unsigned long)ram_first_a,
            (unsigned long)ram_first_b, (unsigned long)ram_mismatch_passes,
            (unsigned long)src_diff_count,
            (long)((src_diff_first == 0xFFFFFFFFU) ? -1L : (long)src_diff_first),
            (unsigned)src_first_value, (unsigned)ram_first_value,
            (unsigned)stable_copy);
        if (iso_n <= 0 || (size_t)iso_n >= sizeof(line) ||
            !send((const uint8_t *)line, (uint16_t)iso_n)) return false;
        if (!stable_copy) {
            int e = snprintf(line, sizeof(line),
                "[SPCM_TRANSFER_ERROR] stage=ISOLATION seq=%lu "
                "passes=%lu first=%08lX second=%08lX last=%08lX "
                "ram_mismatch=%lu src_diff=%lu\r\n",
                (unsigned long)s_orders[k], (unsigned long)copy_passes,
                (unsigned long)memory_crc1, (unsigned long)memory_crc2,
                (unsigned long)last_crc, (unsigned long)ram_mismatch_passes,
                (unsigned long)src_diff_count);
            if (e > 0 && (size_t)e < sizeof(line))
                (void)send((const uint8_t *)line, (uint16_t)e);
            return false;
        }
        /* Global CRC and all individual blocks use this SAME frozen copy. */
        s_crc[k] = crc32_pcm(s_spcm_frozen, SPCM_BYTES);
        int n=snprintf(line,sizeof(line),
            "[SPCM_BEGIN] protocol=ackseq1024 seq=%lu frames=%lu channels=4 "
            "sample_rate=44100 offset=%lu bytes=%lu crc32=%08lX format=pcm16le_planar\r\n",
            (unsigned long)s_orders[k],(unsigned long)SPCM_FRAMES,
            (unsigned long)SPCM_START,(unsigned long)SPCM_BYTES,(unsigned long)s_crc[k]);
        if(n<0 || (size_t)n>=sizeof(line)) return false;
        uint8_t token=0U;
        bool granted=false;
        for(uint32_t announce=0U;announce<10U;announce++) {
            if(announce && !send((const uint8_t*)"\r\n",2U)) return false;
            if(!send((const uint8_t*)line,(uint16_t)n)) return false;
            uint32_t started=HAL_GetTick();
            while((uint32_t)(HAL_GetTick()-started)<1500U) {
                if(HAL_UART_Receive(&huart1,&token,1U,50U)==HAL_OK && token==(uint8_t)'G') {
                    granted=true; break;
                }
            }
            if(granted) break;
        }
        if(!granted) {
            n=snprintf(line,sizeof(line),"[SPCM_TRANSFER_ERROR] stage=START seq=%lu reason=no_G\r\n",(unsigned long)s_orders[k]);
            if(n>0 && (size_t)n<sizeof(line)) (void)send((const uint8_t*)line,(uint16_t)n);
            return false;
        }
        /* 0xFFFFFFFF is the unfinalized state for the concatenated PCM. */
        uint32_t wire_crc_state = 0xFFFFFFFFU;
        uint16_t block=0U;
        for(uint32_t off=0U;off<SPCM_BYTES;++block) {
            uint16_t count=(uint16_t)((SPCM_BYTES-off>SPCM_BLOCK_SIZE)?SPCM_BLOCK_SIZE:(SPCM_BYTES-off));
            /* TRANSPORT FIX: build one immutable header+payload in internal SRAM.
             * Source is the frozen full-snapshot SRAM copy.
             * The packet stays alive until this block is acknowledged. */
            uint8_t packet[12U + SPCM_BLOCK_SIZE];
            packet[0]='S'; packet[1]='P'; packet[2]='B'; packet[3]='2';
            put16(&packet[4],block);
            put16(&packet[6],count);
            memcpy(&packet[12],s_spcm_frozen+off,count);
            put32(&packet[8],crc32_pcm(&packet[12],count));
            /* Update only once per block; retransmissions do not count. */
            wire_crc_state = crc32_update_state(wire_crc_state, &packet[12], count);
            const uint16_t wire_len=(uint16_t)(12U+count);
            bool acknowledged=false;
            for(uint32_t attempt=0U;attempt<SPCM_MAX_RETRIES;attempt++) {
                if(!send(packet,wire_len)) {
                    n=snprintf(line,sizeof(line),
                        "[SPCM_TRANSFER_ERROR] stage=UART seq=%lu block=%u attempt=%lu\r\n",
                        (unsigned long)s_orders[k],(unsigned)block,
                        (unsigned long)attempt);
                    if(n>0 && (size_t)n<sizeof(line))
                        (void)send((const uint8_t*)line,(uint16_t)n);
                    return false;
                }
                if(wait_indexed_ack(block,5000U)) {acknowledged=true;break;}
            }
            if(!acknowledged) {
                n=snprintf(line,sizeof(line),"[SPCM_TRANSFER_ERROR] stage=ACK seq=%lu block=%u\r\n",(unsigned long)s_orders[k],(unsigned)block);
                if(n>0 && (size_t)n<sizeof(line)) (void)send((const uint8_t*)line,(uint16_t)n);
                return false;
            }
            off+=count;
        }
        /* CRC diagnostics: frozen banner/wire and independent SDRAM reread. */
        const uint32_t wire_crc = ~wire_crc_state;
        const uint32_t end_crc = crc32_pcm(pcm, SPCM_BYTES);
        n = snprintf(line, sizeof(line),
            "\r\n[SPCM_CRC_DIAG] seq=%lu begin=%08lX wire=%08lX end=%08lX "
            "bw=%u we=%u be=%u pre1=%08lX pre2=%08lX pre_match=%u "
            "stable=%u passes=%lu\r\n",
            (unsigned long)s_orders[k],
            (unsigned long)s_crc[k], (unsigned long)wire_crc,
            (unsigned long)end_crc,
            (unsigned)(s_crc[k] == wire_crc),
            (unsigned)(wire_crc == end_crc),
            (unsigned)(s_crc[k] == end_crc),
            (unsigned long)memory_crc1, (unsigned long)memory_crc2,
            (unsigned)(memory_crc1 == memory_crc2),
            (unsigned)stable_copy, (unsigned long)copy_passes);
        if (n <= 0 || (size_t)n >= sizeof(line) ||
            !send((const uint8_t*)line, (uint16_t)n)) return false;
        n=snprintf(line,sizeof(line),"\r\n[SPCM_END] protocol=ackseq1024 seq=%lu bytes=%lu crc32=%08lX\r\n",(unsigned long)s_orders[k],(unsigned long)SPCM_BYTES,(unsigned long)s_crc[k]);
        if(n<=0 || (size_t)n>=sizeof(line) || !send((const uint8_t*)line,(uint16_t)n)) return false;
        sent++;
    }
    int n=snprintf(line,sizeof(line),"[SPCM_BATCH_END] count=%lu planned=%lu\r\n",(unsigned long)sent,(unsigned long)SPCM_COUNT);
    return n>0 && (size_t)n<sizeof(line) && send((const uint8_t*)line,(uint16_t)n);
}
