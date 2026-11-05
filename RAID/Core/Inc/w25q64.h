#ifndef W25Q64_H
#define W25Q64_H

#include "stm32f4xx_hal.h"


/*
 * W25Q64FV memory specifications.
 *
 * Capacity: 8 MB
 * Sector:   4 KB
 * Page:     256 bytes
 */
#define W25Q64_CAPACITY_BYTES  (8UL * 1024UL * 1024UL)
#define W25Q64_SECTOR_SIZE     4096UL
#define W25Q64_PAGE_SIZE       256UL


/*
 * Configuration for one W25Q64 flash device.
 *
 * Each flash shares the SPI peripheral but has its own
 * chip-select GPIO pin.
 *
 * spi      -> SPI peripheral used to communicate with the chip.
 * cs_port  -> GPIO port containing its chip-select pin.
 * cs_pin   -> Chip-select pin for this particular chip.
 */
typedef struct
{
    SPI_HandleTypeDef *spi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;

} W25Q64_HandleTypeDef;


/*
 * Read the JEDEC identification bytes from the flash.
 *
 * id: Output array that receives the three identification bytes.
 *
 * For the W25Q64FV, the expected ID is EF 40 17.
 */
HAL_StatusTypeDef W25Q64_ReadID(
    W25Q64_HandleTypeDef *flash,
    uint8_t id[3]);


/*
 * Read Status Register 1.
 *
 * status_byte: Pointer where the received status byte is stored.
 *
 * The status register contains flags such as BUSY and WEL.
 */
HAL_StatusTypeDef W25Q64_ReadStatus(
    W25Q64_HandleTypeDef *flash,
    uint8_t *status_byte);


/*
 * Wait until the flash finishes its current internal operation.
 *
 * timeout_ms: Maximum time to wait, in milliseconds.
 *
 * Returns HAL_OK when the flash is ready, or HAL_TIMEOUT
 * if it remains busy beyond the specified time.
 */
HAL_StatusTypeDef W25Q64_WaitReady(
    W25Q64_HandleTypeDef *flash,
    uint32_t timeout_ms);


/*
 * Read data from the flash into a RAM buffer.
 *
 * address: Starting flash memory address.
 * data:    Destination RAM buffer.
 * length:  Number of bytes to read.
 *
 * Returns HAL_OK when the requested data is read successfully.
 */
HAL_StatusTypeDef W25Q64_Read(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    uint8_t *data,
    uint32_t length);


/*
 * Program data from a RAM buffer into the flash.
 *
 * address: Starting flash memory address.
 * data:    Source RAM buffer containing the data to write.
 * length:  Number of bytes to program.
 *
 * The driver splits writes at page boundaries.
 * The destination memory must be erased where necessary
 * before programming.
 */
HAL_StatusTypeDef W25Q64_Program(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    const uint8_t *data,
    uint32_t length);


/*
 * Erase one 4 KB sector of the flash.
 *
 * sector_address: Starting address of the sector to erase.
 *                 Must be aligned to a sector boundary.
 *
 * After erasing, the bytes in that sector have the value 0xFF.
 */
HAL_StatusTypeDef W25Q64_EraseSector(
    W25Q64_HandleTypeDef *flash,
    uint32_t sector_address);


#endif
