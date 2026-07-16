/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : SAI.c
  * Description        : PRUEBA 1 — HAL_SAI_InitProtocol() I2S estándar 24 bits
  *                      Elimina errores sutiles de FREE_PROTOCOL manual.
  *                      Block_A: MODEMASTER_RX, ASYNCHRONOUS
  *                      Block_B: MODESLAVE_RX,  SYNCHRONOUS
  *                      Ambos usan SAI_I2S_STANDARD + SAI_PROTOCOL_DATASIZE_24BIT
  ******************************************************************************
  */
/* USER CODE END Header */

#include "sai.h"
#include "stdio.h"

SAI_HandleTypeDef hsai_BlockA2;
SAI_HandleTypeDef hsai_BlockB2;
DMA_HandleTypeDef hdma_sai2_a;
DMA_HandleTypeDef hdma_sai2_b;

void MX_SAI2_Init(void)
{
  /* ================================================================
   * BLOCK_A — Maestro asíncrono
   * HAL_SAI_InitProtocol calcula FrameInit y SlotInit automáticamente
   * No se toca CKSTR ni nada manual después
   * ================================================================ */
  hsai_BlockA2.Instance = SAI2_Block_A;
  hsai_BlockA2.Init.AudioMode      = SAI_MODEMASTER_RX;
  hsai_BlockA2.Init.Synchro        = SAI_ASYNCHRONOUS;
  hsai_BlockA2.Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockA2.Init.NoDivider      = SAI_MASTERDIVIDER_ENABLE;
  hsai_BlockA2.Init.MckOverSampling= SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockA2.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockA2.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_44K;
  hsai_BlockA2.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai_BlockA2.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockA2.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockA2.Init.TriState       = SAI_OUTPUT_NOTRELEASED;

  if (HAL_SAI_InitProtocol(&hsai_BlockA2,
                            SAI_I2S_STANDARD,
                            SAI_PROTOCOL_DATASIZE_24BIT,
                            2U) != HAL_OK)
    Error_Handler();

  /* ================================================================
   * BLOCK_B — Esclavo síncrono
   * ================================================================ */
  hsai_BlockB2.Instance = SAI2_Block_B;
  hsai_BlockB2.Init.AudioMode      = SAI_MODESLAVE_RX;
  hsai_BlockB2.Init.Synchro        = SAI_SYNCHRONOUS;
  hsai_BlockB2.Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockB2.Init.NoDivider      = SAI_MASTERDIVIDER_ENABLE;
  hsai_BlockB2.Init.MckOverSampling= SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockB2.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockB2.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai_BlockB2.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockB2.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockB2.Init.TriState       = SAI_OUTPUT_RELEASED;

  if (HAL_SAI_InitProtocol(&hsai_BlockB2,
                            SAI_I2S_STANDARD,
                            SAI_PROTOCOL_DATASIZE_24BIT,
                            2U) != HAL_OK)
    Error_Handler();

  /* Registros efectivos para comparar con FREE_PROTOCOL */
  printf("[PROTO_A] CR1=%08lX CR2=%08lX FRCR=%08lX SLOTR=%08lX\r\n",
         (unsigned long)SAI2_Block_A->CR1,
         (unsigned long)SAI2_Block_A->CR2,
         (unsigned long)SAI2_Block_A->FRCR,
         (unsigned long)SAI2_Block_A->SLOTR);

  printf("[PROTO_B] CR1=%08lX CR2=%08lX FRCR=%08lX SLOTR=%08lX\r\n",
         (unsigned long)SAI2_Block_B->CR1,
         (unsigned long)SAI2_Block_B->CR2,
         (unsigned long)SAI2_Block_B->FRCR,
         (unsigned long)SAI2_Block_B->SLOTR);

  printf("[CKSTR] A=%lu B=%lu\r\n",
         (unsigned long)((SAI2_Block_A->CR1 & SAI_xCR1_CKSTR) != 0U),
         (unsigned long)((SAI2_Block_B->CR1 & SAI_xCR1_CKSTR) != 0U));
}

static uint32_t SAI2_client = 0;

void HAL_SAI_MspInit(SAI_HandleTypeDef* saiHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct;
  if(saiHandle->Instance==SAI2_Block_A)
  {
    if (SAI2_client == 0) __HAL_RCC_SAI2_CLK_ENABLE();
    SAI2_client++;
    GPIO_InitStruct.Pin       = GPIO_PIN_6|GPIO_PIN_5|GPIO_PIN_7;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF10_SAI2;
    HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);
    hdma_sai2_a.Instance                 = DMA1_Stream0;
    hdma_sai2_a.Init.Request             = DMA_REQUEST_SAI2_A;
    hdma_sai2_a.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_sai2_a.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_sai2_a.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_sai2_a.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_sai2_a.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_sai2_a.Init.Mode                = DMA_CIRCULAR;
    hdma_sai2_a.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_sai2_a.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_sai2_a) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(saiHandle,hdmarx,hdma_sai2_a);
    __HAL_LINKDMA(saiHandle,hdmatx,hdma_sai2_a);
  }
  if(saiHandle->Instance==SAI2_Block_B)
  {
    if (SAI2_client == 0) __HAL_RCC_SAI2_CLK_ENABLE();
    SAI2_client++;
    GPIO_InitStruct.Pin       = GPIO_PIN_10;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF10_SAI2;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);
    hdma_sai2_b.Instance                 = DMA1_Stream1;
    hdma_sai2_b.Init.Request             = DMA_REQUEST_SAI2_B;
    hdma_sai2_b.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_sai2_b.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_sai2_b.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_sai2_b.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_sai2_b.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_sai2_b.Init.Mode                = DMA_CIRCULAR;
    hdma_sai2_b.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_sai2_b.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_sai2_b) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(saiHandle,hdmarx,hdma_sai2_b);
    __HAL_LINKDMA(saiHandle,hdmatx,hdma_sai2_b);
  }
}

void HAL_SAI_MspDeInit(SAI_HandleTypeDef* saiHandle)
{
  if(saiHandle->Instance==SAI2_Block_A)
  {
    SAI2_client--;
    if (SAI2_client == 0) __HAL_RCC_SAI2_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOI, GPIO_PIN_6|GPIO_PIN_5|GPIO_PIN_7);
    HAL_DMA_DeInit(saiHandle->hdmarx);
    HAL_DMA_DeInit(saiHandle->hdmatx);
  }
  if(saiHandle->Instance==SAI2_Block_B)
  {
    SAI2_client--;
    if (SAI2_client == 0) __HAL_RCC_SAI2_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOG, GPIO_PIN_10);
    HAL_DMA_DeInit(saiHandle->hdmarx);
    HAL_DMA_DeInit(saiHandle->hdmatx);
  }
}
