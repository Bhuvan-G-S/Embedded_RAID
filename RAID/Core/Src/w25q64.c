#include "w25q64.h"
#include <string.h>

#define W25Q64_CMD_READ_DATA       0x03U
#define W25Q64_CMD_PAGE_PROGRAM    0x02U
#define W25Q64_CMD_SECTOR_ERASE    0x20U
#define W25Q64_CMD_WRITE_ENABLE    0x06U
#define W25Q64_CMD_READ_STATUS1    0x05U
#define W25Q64_CMD_READ_JEDEC_ID   0x9FU

#define W25Q64_DEFAULT_TIMEOUT_MS  1000U
#define W25Q64_PROGRAM_TIMEOUT_MS 100U
#define W25Q64_ERASE_TIMEOUT_MS   1000U
#define W25Q64_STATUS_BUSY_MASK   0x01U
#define W25Q64_STATUS_WEL_MASK    0x02U

#define W25Q64_COMMAND_SIZE       4U
#define W25Q64_MAX_TRANSFER       (W25Q64_COMMAND_SIZE + W25Q64_PAGE_SIZE)

static uint8_t W25Q64_IsValidHandle(const W25Q64_HandleTypeDef *flash)
{
    if (flash == NULL)
    {
        return 0U;
    }

    if ((flash->spi == NULL) || (flash->cs_port == NULL) || (flash->cs_pin == 0U))
    {
        return 0U;
    }

    return 1U;
}

static void W25Q64_Select(const W25Q64_HandleTypeDef *flash)
{
    HAL_GPIO_WritePin(flash->cs_port, flash->cs_pin, GPIO_PIN_RESET);
}

static void W25Q64_Deselect(const W25Q64_HandleTypeDef *flash)
{
    HAL_GPIO_WritePin(flash->cs_port, flash->cs_pin, GPIO_PIN_SET);
}

static void W25Q64_PutAddress(uint8_t *command, uint32_t address)
{
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
}

static HAL_StatusTypeDef W25Q64_WriteEnable(W25Q64_HandleTypeDef *flash)
{
    uint8_t command = W25Q64_CMD_WRITE_ENABLE;
    uint8_t status_byte = 0U;
    HAL_StatusTypeDef result;

    result = W25Q64_WaitReady(flash, W25Q64_DEFAULT_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    W25Q64_Select(flash);
    result = HAL_SPI_Transmit(flash->spi, &command, 1U, 100U);
    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }

    /* Confirm that the flash accepted Write Enable (WEL is status bit 1). */
    result = W25Q64_ReadStatus(flash, &status_byte);
    if (result != HAL_OK)
    {
        return result;
    }

    if ((status_byte & W25Q64_STATUS_WEL_MASK) == 0U)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

HAL_StatusTypeDef W25Q64_ReadID(
    W25Q64_HandleTypeDef *flash,
    uint8_t id[3]
)
{
    uint8_t tx[4] = {W25Q64_CMD_READ_JEDEC_ID, 0xFFU, 0xFFU, 0xFFU};
    uint8_t rx[4] = {0U, 0U, 0U, 0U};
    HAL_StatusTypeDef result;

    if ((!W25Q64_IsValidHandle(flash)) || (id == NULL))
    {
        return HAL_ERROR;
    }

    W25Q64_Select(flash);
    result = HAL_SPI_TransmitReceive(flash->spi, tx, rx, sizeof(tx), 100U);
    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }

    id[0] = rx[1];
    id[1] = rx[2];
    id[2] = rx[3];

    return HAL_OK;
}

HAL_StatusTypeDef W25Q64_ReadStatus(
    W25Q64_HandleTypeDef *flash,
    uint8_t *status_byte
)
{
    uint8_t tx[2] = {W25Q64_CMD_READ_STATUS1, 0xFFU};
    uint8_t rx[2] = {0U, 0U};
    HAL_StatusTypeDef result;

    if ((!W25Q64_IsValidHandle(flash)) || (status_byte == NULL))
    {
        return HAL_ERROR;
    }

    W25Q64_Select(flash);
    result = HAL_SPI_TransmitReceive(flash->spi, tx, rx, sizeof(tx), 100U);
    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }

    *status_byte = rx[1];
    return HAL_OK;
}

HAL_StatusTypeDef W25Q64_WaitReady(
    W25Q64_HandleTypeDef *flash,
    uint32_t timeout_ms
)
{
    uint32_t start_time;
    uint8_t status_byte;
    HAL_StatusTypeDef result;

    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }

    start_time = HAL_GetTick();

    while (1)
    {
        result = W25Q64_ReadStatus(flash, &status_byte);
        if (result != HAL_OK)
        {
            return result;
        }

        if ((status_byte & W25Q64_STATUS_BUSY_MASK) == 0U)
        {
            return HAL_OK;
        }

        if ((HAL_GetTick() - start_time) >= timeout_ms)
        {
            return HAL_TIMEOUT;
        }

        HAL_Delay(1U);
    }
}

HAL_StatusTypeDef W25Q64_Read(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    uint8_t *data,
    uint32_t length
)
{
    uint8_t tx[W25Q64_MAX_TRANSFER];
    uint8_t rx[W25Q64_MAX_TRANSFER];
    HAL_StatusTypeDef result;
    uint32_t offset = 0U;

    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }

    if (length == 0U)
    {
        return HAL_OK;
    }

    if ((data == NULL) || (address >= W25Q64_CAPACITY_BYTES) ||
        (length > (W25Q64_CAPACITY_BYTES - address)))
    {
        return HAL_ERROR;
    }

    result = W25Q64_WaitReady(flash, W25Q64_DEFAULT_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return result;
    }

    /* Read in chunks of at most 256 bytes to keep stack use small. */
    while (offset < length)
    {
        uint32_t chunk = length - offset;
        uint32_t current_address = address + offset;
        uint32_t i;

        if (chunk > W25Q64_PAGE_SIZE)
        {
            chunk = W25Q64_PAGE_SIZE;
        }

        tx[0] = W25Q64_CMD_READ_DATA;
        W25Q64_PutAddress(tx, current_address);

        for (i = 0U; i < chunk; i++)
        {
            tx[W25Q64_COMMAND_SIZE + i] = 0xFFU;
        }

        result = HAL_OK;
        W25Q64_Select(flash);
        result = HAL_SPI_TransmitReceive(
            flash->spi,
            tx,
            rx,
            (uint16_t)(W25Q64_COMMAND_SIZE + chunk),
            500U
        );
        W25Q64_Deselect(flash);

        if (result != HAL_OK)
        {
            return result;
        }

        memcpy(&data[offset], &rx[W25Q64_COMMAND_SIZE], chunk);
        offset += chunk;
    }

    return HAL_OK;
}

HAL_StatusTypeDef W25Q64_Program(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    const uint8_t *data,
    uint32_t length
)
{
    uint8_t tx[W25Q64_MAX_TRANSFER];
    HAL_StatusTypeDef result;
    uint32_t offset = 0U;

    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }

    if (length == 0U)
    {
        return HAL_OK;
    }

    if ((data == NULL) || (address >= W25Q64_CAPACITY_BYTES) ||
        (length > (W25Q64_CAPACITY_BYTES - address)))
    {
        return HAL_ERROR;
    }

    while (offset < length)
    {
        uint32_t current_address = address + offset;
        uint32_t page_remaining = W25Q64_PAGE_SIZE -
                                  (current_address % W25Q64_PAGE_SIZE);
        uint32_t chunk = length - offset;

        if (chunk > page_remaining)
        {
            chunk = page_remaining;
        }

        result = W25Q64_WriteEnable(flash);
        if (result != HAL_OK)
        {
            return result;
        }

        tx[0] = W25Q64_CMD_PAGE_PROGRAM;
        W25Q64_PutAddress(tx, current_address);
        memcpy(&tx[W25Q64_COMMAND_SIZE], &data[offset], chunk);

        W25Q64_Select(flash);
        result = HAL_SPI_Transmit(
            flash->spi,
            tx,
            (uint16_t)(W25Q64_COMMAND_SIZE + chunk),
            500U
        );
        W25Q64_Deselect(flash);

        if (result != HAL_OK)
        {
            return result;
        }

        result = W25Q64_WaitReady(flash, W25Q64_PROGRAM_TIMEOUT_MS);
        if (result != HAL_OK)
        {
            return result;
        }

        offset += chunk;
    }

    return HAL_OK;
}

HAL_StatusTypeDef W25Q64_EraseSector(
    W25Q64_HandleTypeDef *flash,
    uint32_t sector_address
)
{
    uint8_t tx[4] = {W25Q64_CMD_SECTOR_ERASE, 0U, 0U, 0U};
    HAL_StatusTypeDef result;

    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }

    if ((sector_address >= W25Q64_CAPACITY_BYTES) ||
        ((sector_address % W25Q64_SECTOR_SIZE) != 0U))
    {
        return HAL_ERROR;
    }

    result = W25Q64_WriteEnable(flash);
    if (result != HAL_OK)
    {
        return result;
    }

    W25Q64_PutAddress(tx, sector_address);

    W25Q64_Select(flash);
    result = HAL_SPI_Transmit(flash->spi, tx, sizeof(tx), 100U);
    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }

    /* A 4 KiB sector erase can take hundreds of milliseconds. */
    return W25Q64_WaitReady(flash, W25Q64_ERASE_TIMEOUT_MS);
}
