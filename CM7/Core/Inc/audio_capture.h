/**
 * @file audio_capture.h
 * @brief SAI + DMA Audio Capture Driver for STM32H7
 * 
 * Captures 4-channel audio from 2 SAI blocks (SAI1_A + SAI1_B)
 * with hardware multiplexing via L/R pins.
 * 
 * Pinout:
 *   SAI1_A: Mic1 (L) + Mic2 (R) on SD_A
 *   SAI1_B: Mic3 (L) + Mic4 (R) on SD_B
 *   Shared: SCK (PE5), FS (PE4)
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>
#include <string.h>
#include "main.h"

/* ============ CONFIGURATION ============ */
#define AUDIO_SAMPLE_RATE       44100       // Hz (must match SAI config)
#define AUDIO_CHANNELS          4           // 4 microphones
#define AUDIO_BUFFER_SIZE       512         // samples per buffer
#define AUDIO_DMA_BUFFER_SIZE   (AUDIO_BUFFER_SIZE * 2)  // stereo pairs

/* Data format */
#define AUDIO_SAMPLE_BITS       32          // 32-bit per sample
#define AUDIO_SAMPLE_BYTES      (AUDIO_SAMPLE_BITS / 8)

/* ============ DATA STRUCTURES ============ */

/**
 * @brief Audio buffer states
 */
typedef enum {
    AUDIO_BUFFER_EMPTY = 0,
    AUDIO_BUFFER_HALF = 1,      // Half-transfer (first half ready)
    AUDIO_BUFFER_FULL = 2       // Full transfer (second half ready)
} AudioBufferState;

/**
 * @brief Audio capture context
 */
typedef struct {
    // DMA buffers (raw SAI data)
    uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];  // SAI_A data (Mic1 + Mic2)
    uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];  // SAI_B data (Mic3 + Mic4)
    
    // Deinterleaved buffers (processed audio)
    int32_t ch0[AUDIO_BUFFER_SIZE];  // Microphone 1
    int32_t ch1[AUDIO_BUFFER_SIZE];  // Microphone 2
    int32_t ch2[AUDIO_BUFFER_SIZE];  // Microphone 3
    int32_t ch3[AUDIO_BUFFER_SIZE];  // Microphone 4
    
    // State tracking
    volatile uint8_t dma_half_complete;  // Flag: first half ready
    volatile uint8_t dma_full_complete;  // Flag: second half ready
    volatile AudioBufferState buffer_state;
    
    // Statistics (optional)
    uint32_t frame_count;
    uint32_t error_count;
    
} AudioCaptureContext;

/* ============ PUBLIC API ============ */

/**
 * @brief Initialize audio capture system
 * @return 0 on success, -1 on error
 */
int audio_capture_init(AudioCaptureContext *ctx);

/**
 * @brief Start capturing audio (enable SAI + DMA)
 * @return 0 on success, -1 on error
 */
int audio_capture_start(AudioCaptureContext *ctx);

/**
 * @brief Stop audio capture
 * @return 0 on success, -1 on error
 */
int audio_capture_stop(AudioCaptureContext *ctx);

/**
 * @brief Poll for new audio data
 * 
 * Returns immediately with buffer state.
 * Must be called in main loop to check if audio is ready.
 * 
 * @param ctx Audio context
 * @return AUDIO_BUFFER_EMPTY, AUDIO_BUFFER_HALF, or AUDIO_BUFFER_FULL
 */
AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx);

/**
 * @brief Get pointer to deinterleaved channel data
 * 
 * @param ctx Audio context
 * @param channel (0-3) which microphone
 * @return pointer to int32_t array[AUDIO_BUFFER_SIZE]
 */
int32_t* audio_capture_get_channel(AudioCaptureContext *ctx, uint8_t channel);

/**
 * @brief Manual deinterleave (called by callbacks)
 * 
 * Separates raw SAI stereo data into 4 individual channels
 * 
 * @param ctx Audio context
 * @param half (0=first half, 1=second half)
 */
void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half);

/**
 * @brief Get frame statistics
 * 
 * @param ctx Audio context
 * @param frames_processed pointer to output
 * @param errors pointer to output
 */
void audio_capture_get_stats(AudioCaptureContext *ctx, 
                             uint32_t *frames_processed, 
                             uint32_t *errors);

/* ============ GLOBAL CONTEXT ============ */
extern AudioCaptureContext g_audio_ctx;

#endif /* AUDIO_CAPTURE_H */
