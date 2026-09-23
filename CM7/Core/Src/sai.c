#include "sai.h"

SAI_HandleTypeDef hsai_BlockA2;
DMA_HandleTypeDef hdma_sai2_a;

void MX_SAI2_Init(void)
{
    hsai_BlockA2.Instance = SAI2_Block_A;
    hsai_BlockA2.Init.AudioMode = SAI_MODEMASTER_RX;
    hsai_BlockA2.Init.Synchro = SAI_ASYNCHRONOUS;
    hsai_BlockA2.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLE;
    hsai_BlockA2.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
    hsai_BlockA2.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_DISABLE;
    hsai_BlockA2.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_EMPTY;
    hsai_BlockA2.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_44K;
    hsai_BlockA2.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
    hsai_BlockA2.Init.MonoStereoMode = SAI_STEREOMODE;
    hsai_BlockA2.Init.CompandingMode = SAI_NOCOMPANDING;
    hsai_BlockA2.Init.TriState = SAI_OUTPUT_NOTRELEASED;

    if (HAL_SAI_InitProtocol(
            &hsai_BlockA2,
            SAI_I2S_STANDARD,
            SAI_PROTOCOL_DATASIZE_24BIT,
            2U) != HAL_OK)
    {
        Error_Handler();
    }
}

void HAL_SAI_MspInit(SAI_HandleTypeDef *saiHandle)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (saiHandle->Instance == SAI2_Block_A)
    {
        __HAL_RCC_SAI2_CLK_ENABLE();
        __HAL_RCC_GPIOI_CLK_ENABLE();

        GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF10_SAI2;
        HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);

        hdma_sai2_a.Instance = DMA1_Stream0;
        hdma_sai2_a.Init.Request = DMA_REQUEST_SAI2_A;
        hdma_sai2_a.Init.Direction = DMA_PERIPH_TO_MEMORY;
        hdma_sai2_a.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_sai2_a.Init.MemInc = DMA_MINC_ENABLE;
        hdma_sai2_a.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
        hdma_sai2_a.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
        hdma_sai2_a.Init.Mode = DMA_CIRCULAR;
        hdma_sai2_a.Init.Priority = DMA_PRIORITY_VERY_HIGH;
        hdma_sai2_a.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&hdma_sai2_a) != HAL_OK)
        {
            Error_Handler();
        }

        __HAL_LINKDMA(saiHandle, hdmarx, hdma_sai2_a);
    }
}

void HAL_SAI_MspDeInit(SAI_HandleTypeDef *saiHandle)
{
    if (saiHandle->Instance == SAI2_Block_A)
    {
        __HAL_RCC_SAI2_CLK_DISABLE();

        HAL_GPIO_DeInit(
            GPIOI,
            GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7
        );

        HAL_DMA_DeInit(saiHandle->hdmarx);
    }
}
