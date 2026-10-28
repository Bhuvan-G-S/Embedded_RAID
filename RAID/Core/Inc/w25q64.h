#ifndef W25Q64_H
#define W25Q64_H

#include "stm32f4xx_hal.h"

#define W25Q64_CAPACITY_BYTES  (8UL * 1024UL * 1024UL)
#define W25Q64_SECTOR_SIZE     4096UL
#define W25Q64_PAGE_SIZE       256UL

typedef struct
{
    SPI_HandleTypeDef *spi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
} W25Q64_HandleTypeDef;

HAL_StatusTypeDef W25Q64_ReadID(
    W25Q64_HandleTypeDef *flash,
    uint8_t id[3]);

HAL_StatusTypeDef W25Q64_ReadStatus(
    W25Q64_HandleTypeDef *flash,
    uint8_t *status_byte);

HAL_StatusTypeDef W25Q64_WaitReady(
    W25Q64_HandleTypeDef *flash,
    uint32_t timeout_ms);

HAL_StatusTypeDef W25Q64_Read(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    uint8_t *data,
    uint32_t length);

HAL_StatusTypeDef W25Q64_Program(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    const uint8_t *data,
    uint32_t length);

HAL_StatusTypeDef W25Q64_EraseSector(
    W25Q64_HandleTypeDef *flash,
    uint32_t sector_address);

#endif
