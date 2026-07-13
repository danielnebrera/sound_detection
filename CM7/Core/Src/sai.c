/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : SAI.c
  * Description        : Configuración estable HAL_SAI_Init() manual
  *                      Mic2(SAI_B/SEL=VCC) + Mic4(SAI_A/SEL=VCC) activos
  *                      ACTIVE_LOW + RISINGEDGE + BEFOREFIRSTBIT + DATASIZE_32
  *                      FIFOThreshold = EMPTY  DMA Priority = LOW
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
   * Mic3(PI6, SEL=GND, slot par)  — no activo
   * Mic4(PI6, SEL=VCC, slot impar) — activo ✅
   * ================================================================ */
  hsai_BlockA2.Instance = SAI2_Block_A;

  hsai_BlockA2.Init.AudioMode      = SAI_MODEMASTER_RX;
  hsai_BlockA2.Init.Synchro        = SAI_ASYNCHRONOUS;
  hsai_BlockA2.Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockA2.Init.NoDivider      = SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockA2.Init.MckOverSampling= SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockA2.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockA2.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_44K;
  hsai_BlockA2.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai_BlockA2.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockA2.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockA2.Init.TriState       = SAI_OUTPUT_NOTRELEASED;

  hsai_BlockA2.Init.Protocol       = SAI_FREE_PROTOCOL;
  hsai_BlockA2.Init.DataSize       = SAI_DATASIZE_32;
  hsai_BlockA2.Init.FirstBit       = SAI_FIRSTBIT_MSB;
  hsai_BlockA2.Init.ClockStrobing  = SAI_CLOCKSTROBING_FALLINGEDGE;

  hsai_BlockA2.FrameInit.FrameLength       = 64;
  hsai_BlockA2.FrameInit.ActiveFrameLength = 32;
  hsai_BlockA2.FrameInit.FSDefinition      = SAI_FS_CHANNEL_IDENTIFICATION;
  hsai_BlockA2.FrameInit.FSPolarity        = SAI_FS_ACTIVE_LOW;
  hsai_BlockA2.FrameInit.FSOffset          = SAI_FS_BEFOREFIRSTBIT;

  hsai_BlockA2.SlotInit.FirstBitOffset = 0;
  hsai_BlockA2.SlotInit.SlotSize       = SAI_SLOTSIZE_32B;
  hsai_BlockA2.SlotInit.SlotNumber     = 2;
  hsai_BlockA2.SlotInit.SlotActive     = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;

  if (HAL_SAI_Init(&hsai_BlockA2) != HAL_OK)
    Error_Handler();

  /* ================================================================
   * BLOCK_B — Esclavo síncrono
   * Mic1(PG10, SEL=GND, slot par)  — no activo
   * Mic2(PG10, SEL=VCC, slot impar) — activo ✅
   * ================================================================ */
  hsai_BlockB2.Instance = SAI2_Block_B;

  hsai_BlockB2.Init.AudioMode      = SAI_MODESLAVE_RX;
  hsai_BlockB2.Init.Synchro        = SAI_SYNCHRONOUS;
  hsai_BlockB2.Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockB2.Init.MckOverSampling= SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockB2.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockB2.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai_BlockB2.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockB2.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockB2.Init.TriState       = SAI_OUTPUT_RELEASED;

  hsai_BlockB2.Init.Protocol       = SAI_FREE_PROTOCOL;
  hsai_BlockB2.Init.DataSize       = SAI_DATASIZE_32;
  hsai_BlockB2.Init.FirstBit       = SAI_FIRSTBIT_MSB;
  hsai_BlockB2.Init.ClockStrobing  = SAI_CLOCKSTROBING_FALLINGEDGE;

  hsai_BlockB2.FrameInit.FrameLength       = 64;
  hsai_BlockB2.FrameInit.ActiveFrameLength = 32;
  hsai_BlockB2.FrameInit.FSDefinition      = SAI_FS_CHANNEL_IDENTIFICATION;
  hsai_BlockB2.FrameInit.FSPolarity        = SAI_FS_ACTIVE_LOW;
  hsai_BlockB2.FrameInit.FSOffset          = SAI_FS_BEFOREFIRSTBIT;

  hsai_BlockB2.SlotInit.FirstBitOffset = 0;
  hsai_BlockB2.SlotInit.SlotSize       = SAI_SLOTSIZE_32B;
  hsai_BlockB2.SlotInit.SlotNumber     = 2;
  hsai_BlockB2.SlotInit.SlotActive     = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;

  if (HAL_SAI_Init(&hsai_BlockB2) != HAL_OK)
    Error_Handler();
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
