/**
 * @file audio_capture.c
 * @brief SAI + DMA Audio Capture Implementation for STM32H7
 */

#include "audio_capture.h"


/* Global audio context */
AudioCaptureContext g_audio_ctx = {0};

/* USER CODE BEGIN 0 */

/* External SAI Handles (defined in sai.c) */
extern SAI_HandleTypeDef hsai_BlockA1;
extern SAI_HandleTypeDef hsai_BlockB1;

/* USER CODE END 0 */

/**
 * @brief Initialize audio capture system
 */
int audio_capture_init(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    
    /* Clear buffers */
    memset(ctx->dma_buf_a, 0, sizeof(ctx->dma_buf_a));
    memset(ctx->dma_buf_b, 0, sizeof(ctx->dma_buf_b));
    memset(ctx->ch0, 0, sizeof(ctx->ch0));
    memset(ctx->ch1, 0, sizeof(ctx->ch1));
    memset(ctx->ch2, 0, sizeof(ctx->ch2));
    memset(ctx->ch3, 0, sizeof(ctx->ch3));
    
    /* Clear flags */
    ctx->dma_half_complete = 0;
    ctx->dma_full_complete = 0;
    ctx->buffer_state = AUDIO_BUFFER_EMPTY;
    
    /* Statistics */
    ctx->frame_count = 0;
    ctx->error_count = 0;
    
    return 0;
}

/**
 * @brief Start capturing audio
 */
int audio_capture_start(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    
    /* Start SAI Block A (Master Receiver) */
    if (HAL_SAI_Receive_DMA(&hsai_BlockA1, 
                            (uint8_t *)ctx->dma_buf_a, 
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -1;
    }
    
    /* Start SAI Block B (Slave Receiver, synchronized with A) */
    if (HAL_SAI_Receive_DMA(&hsai_BlockB1, 
                            (uint8_t *)ctx->dma_buf_b, 
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -1;
    }
    
    return 0;
}

/**
 * @brief Stop audio capture
 */
int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    
    HAL_SAI_DMAStop(&hsai_BlockA1);
    HAL_SAI_DMAStop(&hsai_BlockB1);
    
    return 0;
}

/**
 * @brief Deinterleave SAI stereo data into 4 channels
 * 
 * SAI_A output: [Mic1_L, Mic2_R, Mic1_L, Mic2_R, ...]
 * SAI_B output: [Mic3_L, Mic4_R, Mic3_L, Mic4_R, ...]
 * 
 * After deinterleave:
 *   ch0: [Mic1, Mic1, Mic1, ...] (all left from SAI_A)
 *   ch1: [Mic2, Mic2, Mic2, ...] (all right from SAI_A)
 *   ch2: [Mic3, Mic3, Mic3, ...] (all left from SAI_B)
 *   ch3: [Mic4, Mic4, Mic4, ...] (all right from SAI_B)
 */
void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half)
{
    if (!ctx) return;
    
    uint32_t offset = (half == 0) ? 0 : AUDIO_BUFFER_SIZE;
    uint32_t src_offset = offset * 2;  // stereo samples are interleaved
    
    /* Deinterleave SAI_A (Mic1 + Mic2) */
    for (uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        uint32_t stereo_idx = (src_offset + i * 2);
        
        /* Mic1 (Left channel from SAI_A) */
        ctx->ch0[offset + i] = (int32_t)ctx->dma_buf_a[stereo_idx];
        
        /* Mic2 (Right channel from SAI_A) */
        ctx->ch1[offset + i] = (int32_t)ctx->dma_buf_a[stereo_idx + 1];
    }
    
    /* Deinterleave SAI_B (Mic3 + Mic4) */
    for (uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        uint32_t stereo_idx = (src_offset + i * 2);
        
        /* Mic3 (Left channel from SAI_B) */
        ctx->ch2[offset + i] = (int32_t)ctx->dma_buf_b[stereo_idx];
        
        /* Mic4 (Right channel from SAI_B) */
        ctx->ch3[offset + i] = (int32_t)ctx->dma_buf_b[stereo_idx + 1];
    }
}

/**
 * @brief Poll for new audio data
 */
AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx)
{
    if (!ctx) return AUDIO_BUFFER_EMPTY;
    
    /* Check if buffer is ready (flags set by callbacks) */
    
    if (ctx->dma_half_complete)
    {
        ctx->dma_half_complete = 0;
        audio_capture_deinterleave(ctx, 0);  // First half
        ctx->buffer_state = AUDIO_BUFFER_HALF;
        ctx->frame_count++;
        return AUDIO_BUFFER_HALF;
    }
    
    if (ctx->dma_full_complete)
    {
        ctx->dma_full_complete = 0;
        audio_capture_deinterleave(ctx, 1);  // Second half
        ctx->buffer_state = AUDIO_BUFFER_FULL;
        ctx->frame_count++;
        return AUDIO_BUFFER_FULL;
    }
    
    return AUDIO_BUFFER_EMPTY;
}

/**
 * @brief Get channel data pointer
 */
int32_t* audio_capture_get_channel(AudioCaptureContext *ctx, uint8_t channel)
{
    if (!ctx || channel >= AUDIO_CHANNELS) return NULL;
    
    switch (channel)
    {
        case 0: return ctx->ch0;
        case 1: return ctx->ch1;
        case 2: return ctx->ch2;
        case 3: return ctx->ch3;
        default: return NULL;
    }
}

/**
 * @brief Get statistics
 */
void audio_capture_get_stats(AudioCaptureContext *ctx, 
                             uint32_t *frames_processed, 
                             uint32_t *errors)
{
    if (ctx)
    {
        if (frames_processed) *frames_processed = ctx->frame_count;
        if (errors) *errors = ctx->error_count;
    }
}

/* ============ DMA CALLBACKS ============ */

/**
 * @brief SAI Half-Transfer Complete Callback
 * Called when DMA transfers first half of buffer
 */
void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA1)
    {
        /* Flag that first half is ready (both SAI_A and SAI_B sync) */
        g_audio_ctx.dma_half_complete = 1;
    }
}

/**
 * @brief SAI Full-Transfer Complete Callback
 * Called when DMA transfers second half of buffer
 */
void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA1)
    {
        /* Flag that second half is ready */
        g_audio_ctx.dma_full_complete = 1;
    }
}

/**
 * @brief SAI Error Callback
 */
void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    g_audio_ctx.error_count++;
    /* Optionally restart capture */
    // audio_capture_stop(&g_audio_ctx);
    // audio_capture_start(&g_audio_ctx);
}