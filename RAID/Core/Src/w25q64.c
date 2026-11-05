#include "w25q64.h"
#include <string.h>


/*
 * W25Q64FV command opcodes.
 *
 * These are the commands understood by the flash chip.
 * The STM32 sends the opcode over SPI to request an operation.
 */
#define W25Q64_CMD_READ_DATA       0x03U
#define W25Q64_CMD_PAGE_PROGRAM    0x02U
#define W25Q64_CMD_SECTOR_ERASE    0x20U
#define W25Q64_CMD_WRITE_ENABLE    0x06U
#define W25Q64_CMD_READ_STATUS1    0x05U
#define W25Q64_CMD_READ_JEDEC_ID   0x9FU


/*
 * Maximum time allowed for different flash operations.
 *
 * The flash performs programming and erasing internally
 * after receiving the SPI command. These timeouts prevent
 * the firmware from waiting indefinitely.
 */
#define W25Q64_DEFAULT_TIMEOUT_MS  1000U
#define W25Q64_PROGRAM_TIMEOUT_MS  100U
#define W25Q64_ERASE_TIMEOUT_MS    1000U


/*
 * Masks used to check individual bits in Status Register 1.
 *
 * BUSY (bit 0):
 * 1 = Flash is performing an internal operation.
 * 0 = Flash is ready.
 *
 * WEL (bit 1):
 * 1 = Write Enable Latch is set.
 * 0 = Write and erase operations are not enabled.
 */
#define W25Q64_STATUS_BUSY_MASK    0x01U
#define W25Q64_STATUS_WEL_MASK     0x02U


/*
 * A normal read or page-program command contains:
 * 1 byte opcode + 3 bytes memory address.
 */
#define W25Q64_COMMAND_SIZE        4U

/*
 * Maximum SPI transfer size:
 * 4 command/address bytes + 256 data bytes.
 *
 * The temporary transfer buffers use this size so that
 * one transaction can carry a complete page of data.
 */
#define W25Q64_MAX_TRANSFER \
    (W25Q64_COMMAND_SIZE + W25Q64_PAGE_SIZE)


/*
 * Check whether a flash handle contains a valid configuration.
 *
 * Each flash handle must contain:
 * - A valid SPI handle.
 * - A GPIO port for its chip-select pin.
 * - A valid chip-select pin.
 *
 * Returns 1 if the handle looks valid, otherwise 0.
 */
static uint8_t W25Q64_IsValidHandle(
    const W25Q64_HandleTypeDef *flash)
{
    if (flash == NULL)
    {
        return 0U;
    }

    if ((flash->spi == NULL) ||
        (flash->cs_port == NULL) ||
        (flash->cs_pin == 0U))
    {
        return 0U;
    }

    return 1U;
}


/*
 * Select one flash device by pulling its CS pin LOW.
 *
 * The four flash chips share SPI1, so their individual CS
 * pins determine which chip responds to the SPI communication.
 *
 * CS is active-low:
 * LOW  = Selected
 * HIGH = Deselected
 */
static void W25Q64_Select(
    const W25Q64_HandleTypeDef *flash)
{
    HAL_GPIO_WritePin(
        flash->cs_port,
        flash->cs_pin,
        GPIO_PIN_RESET);
}


/* Deselect a flash device by setting its CS pin HIGH. */
static void W25Q64_Deselect(
    const W25Q64_HandleTypeDef *flash)
{
    HAL_GPIO_WritePin(
        flash->cs_port,
        flash->cs_pin,
        GPIO_PIN_SET);
}


/*
 * Split a 32-bit memory address into the three address bytes
 * expected by the W25Q64FV SPI protocol.
 *
 * The command buffer layout is:
 *
 * command[0] = Opcode
 * command[1] = Address bits 23-16
 * command[2] = Address bits 15-8
 * command[3] = Address bits 7-0
 *
 * Example:
 * Address 0x001000 becomes 00 10 00.
 */
static void W25Q64_PutAddress(
    uint8_t *command,
    uint32_t address)
{
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
}


/*
 * Enable programming or erasing on the selected flash chip.
 *
 * The W25Q64 requires the Write Enable Latch (WEL) to be set
 * before accepting Page Program and Sector Erase commands.
 *
 * Sequence:
 * 1. Wait until the flash is ready.
 * 2. Send the Write Enable command (0x06).
 * 3. Read Status Register 1.
 * 4. Check whether the WEL bit is set.
 *
 * Returns HAL_OK only when Write Enable is confirmed.
 */
static HAL_StatusTypeDef W25Q64_WriteEnable(
    W25Q64_HandleTypeDef *flash)
{
    uint8_t command = W25Q64_CMD_WRITE_ENABLE;
    uint8_t status_byte = 0U;
    HAL_StatusTypeDef result;


    /* Make sure the flash has finished its previous operation. */
    result = W25Q64_WaitReady(
        flash,
        W25Q64_DEFAULT_TIMEOUT_MS);

    if (result != HAL_OK)
    {
        return result;
    }


    /* Send the Write Enable opcode (0x06). */
    W25Q64_Select(flash);

    result = HAL_SPI_Transmit(
        flash->spi,
        &command,
        1U,
        100U);

    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }


    /*
     * Read the status register to confirm that the Write
     * Enable Latch has actually been set.
     */
    result = W25Q64_ReadStatus(flash, &status_byte);

    if (result != HAL_OK)
    {
        return result;
    }


    /* Check bit 1 (WEL) using the bitwise AND operator. */
    if ((status_byte & W25Q64_STATUS_WEL_MASK) == 0U)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}


/*
 * Read the JEDEC identification bytes from a flash device.
 *
 * The expected W25Q64FV response is:
 * EF 40 17
 *
 * The three bytes are returned through the 'id' array.
 *
 * The first received byte is ignored because it corresponds
 * to the command transmission.
 */
HAL_StatusTypeDef W25Q64_ReadID(
    W25Q64_HandleTypeDef *flash,
    uint8_t id[3])
{
    /*
     * 0x9F requests the JEDEC ID.
     * The remaining bytes generate the clocks needed to
     * receive the three identification bytes.
     */
    uint8_t tx[4] = {
        W25Q64_CMD_READ_JEDEC_ID,
        0xFFU, 0xFFU, 0xFFU
    };

    uint8_t rx[4] = {0U, 0U, 0U, 0U};
    HAL_StatusTypeDef result;


    /* Check the handle and the output array before using them. */
    if ((!W25Q64_IsValidHandle(flash)) || (id == NULL))
    {
        return HAL_ERROR;
    }


    /* Select the flash and transmit/receive the ID command. */
    W25Q64_Select(flash);

    result = HAL_SPI_TransmitReceive(
        flash->spi,
        tx,
        rx,
        sizeof(tx),
        100U);

    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }


    /*
     * Copy the three ID bytes from the received SPI buffer.
     *
     * rx[0] is ignored because it was received during
     * transmission of the 0x9F command.
     */
    id[0] = rx[1];
    id[1] = rx[2];
    id[2] = rx[3];

    return HAL_OK;
}


/*
 * Read Status Register 1 from the flash.
 *
 * The status register contains flags such as:
 * - BUSY: tells us whether an internal operation is running.
 * - WEL : tells us whether writing has been enabled.
 *
 * The received status byte is stored in the caller's variable
 * through the status_byte pointer.
 */
HAL_StatusTypeDef W25Q64_ReadStatus(
    W25Q64_HandleTypeDef *flash,
    uint8_t *status_byte)
{
    /*
     * 0x05 requests Status Register 1.
     * 0xFF generates the clocks needed to receive the status byte.
     */
    uint8_t tx[2] = {
        W25Q64_CMD_READ_STATUS1,
        0xFFU
    };

    uint8_t rx[2] = {0U, 0U};
    HAL_StatusTypeDef result;


    if ((!W25Q64_IsValidHandle(flash)) ||
        (status_byte == NULL))
    {
        return HAL_ERROR;
    }


    W25Q64_Select(flash);

    result = HAL_SPI_TransmitReceive(
        flash->spi,
        tx,
        rx,
        sizeof(tx),
        100U);

    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }


    /* Store the received status byte in the caller's variable. */
    *status_byte = rx[1];

    return HAL_OK;
}


/*
 * Wait until the flash finishes its current internal operation.
 *
 * The function repeatedly reads Status Register 1 and checks
 * the BUSY bit.
 *
 * It returns:
 * HAL_OK      -> Flash is ready.
 * HAL_TIMEOUT -> Flash remained busy for too long.
 * Other HAL status -> Status-register communication failed.
 *
 * timeout_ms determines the maximum time to wait.
 */
HAL_StatusTypeDef W25Q64_WaitReady(
    W25Q64_HandleTypeDef *flash,
    uint32_t timeout_ms)
{
    uint32_t start_time;
    uint8_t status_byte;
    HAL_StatusTypeDef result;


    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }


    /* Record the time when polling begins. */
    start_time = HAL_GetTick();


    while (1)
    {
        /* Read the current flash status. */
        result = W25Q64_ReadStatus(
            flash,
            &status_byte);

        if (result != HAL_OK)
        {
            return result;
        }


        /*
         * Check bit 0 (BUSY).
         * If it is zero, the flash has finished its operation.
         */
        if ((status_byte & W25Q64_STATUS_BUSY_MASK) == 0U)
        {
            return HAL_OK;
        }


        /*
         * Check how much time has passed.
         * Stop polling if the requested timeout is exceeded.
         */
        if ((HAL_GetTick() - start_time) >= timeout_ms)
        {
            return HAL_TIMEOUT;
        }


        /* Wait briefly before checking the status again. */
        HAL_Delay(1U);
    }
}


/*
 * Read data from a specified flash address into a RAM buffer.
 *
 * Parameters:
 * flash   -> Flash device to read from.
 * address -> Starting memory address.
 * data    -> RAM buffer that receives the data.
 * length  -> Number of bytes to read.
 *
 * Reads are split into chunks of at most 256 bytes so they
 * fit inside the temporary SPI transfer buffers.
 */
HAL_StatusTypeDef W25Q64_Read(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    uint8_t *data,
    uint32_t length)
{
    /*
     * tx contains the command, address and dummy bytes.
     * rx receives the command-phase response and flash data.
     */
    uint8_t tx[W25Q64_MAX_TRANSFER];
    uint8_t rx[W25Q64_MAX_TRANSFER];

    HAL_StatusTypeDef result;
    uint32_t offset = 0U;


    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }


    /* Nothing to read, so no SPI operation is required. */
    if (length == 0U)
    {
        return HAL_OK;
    }


    /*
     * Check that the data buffer is valid and the requested
     * address range stays within the flash capacity.
     */
    if ((data == NULL) ||
        (address >= W25Q64_CAPACITY_BYTES) ||
        (length > (W25Q64_CAPACITY_BYTES - address)))
    {
        return HAL_ERROR;
    }


    /* Ensure the flash is ready before starting the read. */
    result = W25Q64_WaitReady(
        flash,
        W25Q64_DEFAULT_TIMEOUT_MS);

    if (result != HAL_OK)
    {
        return result;
    }


    /*
     * Read the requested data in chunks.
     * offset tracks how many bytes have already been received.
     */
    while (offset < length)
    {
        uint32_t chunk = length - offset;
        uint32_t current_address = address + offset;
        uint32_t i;


        /* Limit this SPI transaction to the available buffer size. */
        if (chunk > W25Q64_PAGE_SIZE)
        {
            chunk = W25Q64_PAGE_SIZE;
        }


        /* Place the Read Data opcode at the start of the buffer. */
        tx[0] = W25Q64_CMD_READ_DATA;


        /* Add the three memory address bytes after the opcode. */
        W25Q64_PutAddress(
            tx,
            current_address);


        /*
         * SPI is full duplex: the STM32 must transmit bytes
         * to generate clock pulses while receiving flash data.
         *
         * 0xFF is used as dummy transmit data during the read.
         */
        for (i = 0U; i < chunk; i++)
        {
            tx[W25Q64_COMMAND_SIZE + i] = 0xFFU;
        }


        W25Q64_Select(flash);


        /*
         * Transfer the four command/address bytes followed
         * by the dummy bytes. The flash returns its data in rx.
         */
        result = HAL_SPI_TransmitReceive(
            flash->spi,
            tx,
            rx,
            (uint16_t)(W25Q64_COMMAND_SIZE + chunk),
            500U);

        W25Q64_Deselect(flash);

        if (result != HAL_OK)
        {
            return result;
        }


        /*
         * The first four received bytes correspond to the
         * command/address phase. Actual memory data begins at rx[4].
         *
         * Copy the received data into the caller's RAM buffer.
         */
        memcpy(
            &data[offset],
            &rx[W25Q64_COMMAND_SIZE],
            chunk);


        /* Move to the next part of the requested address range. */
        offset += chunk;
    }

    return HAL_OK;
}


/*
 * Program data into the flash using the Page Program command.
 *
 * Parameters:
 * flash   -> Flash device to write to.
 * address -> Starting memory address.
 * data    -> Source data in RAM.
 * length  -> Number of bytes to program.
 *
 * A W25Q64FV page is 256 bytes.
 *
 * A Page Program operation must not cross a page boundary.
 * This function splits larger writes into page-safe chunks.
 *
 * The destination must have been erased where necessary before
 * programming. This function does not erase memory itself.
 */
HAL_StatusTypeDef W25Q64_Program(
    W25Q64_HandleTypeDef *flash,
    uint32_t address,
    const uint8_t *data,
    uint32_t length)
{
    /* Holds the Page Program command, address and data bytes. */
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


    /* Check the input buffer and requested memory range. */
    if ((data == NULL) ||
        (address >= W25Q64_CAPACITY_BYTES) ||
        (length > (W25Q64_CAPACITY_BYTES - address)))
    {
        return HAL_ERROR;
    }


    /*
     * Continue until the entire requested length has been programmed.
     */
    while (offset < length)
    {
        uint32_t current_address = address + offset;


        /*
         * Calculate the number of bytes remaining in the current page.
         *
         * Example:
         * Page size = 256 bytes.
         * Current offset inside page = 240.
         * Remaining space = 256 - 240 = 16 bytes.
         */
        uint32_t page_remaining =
            W25Q64_PAGE_SIZE -
            (current_address % W25Q64_PAGE_SIZE);


        /* Start with all remaining data, then limit it to page space. */
        uint32_t chunk = length - offset;

        if (chunk > page_remaining)
        {
            chunk = page_remaining;
        }


        /*
         * Enable programming before sending the Page Program command.
         * The WEL bit must be set for the write to be accepted.
         */
        result = W25Q64_WriteEnable(flash);

        if (result != HAL_OK)
        {
            return result;
        }


        /* Place the Page Program opcode and starting address in tx. */
        tx[0] = W25Q64_CMD_PAGE_PROGRAM;

        W25Q64_PutAddress(
            tx,
            current_address);


        /* Copy this page's data after the four command/address bytes. */
        memcpy(
            &tx[W25Q64_COMMAND_SIZE],
            &data[offset],
            chunk);


        W25Q64_Select(flash);


        /* Transmit the Page Program command, address and data. */
        result = HAL_SPI_Transmit(
            flash->spi,
            tx,
            (uint16_t)(W25Q64_COMMAND_SIZE + chunk),
            500U);

        W25Q64_Deselect(flash);

        if (result != HAL_OK)
        {
            return result;
        }


        /*
         * Page Program starts an internal flash operation.
         * Wait until the BUSY bit clears before programming
         * another page.
         */
        result = W25Q64_WaitReady(
            flash,
            W25Q64_PROGRAM_TIMEOUT_MS);

        if (result != HAL_OK)
        {
            return result;
        }


        /* Advance the source and destination positions. */
        offset += chunk;
    }

    return HAL_OK;
}


/*
 * Erase one 4 KB sector of the selected flash device.
 *
 * Parameters:
 * flash          -> Flash device to erase.
 * sector_address -> Starting address of the sector.
 *
 * The address must be aligned to a 4 KB boundary.
 *
 * Sequence:
 * 1. Validate the handle and address.
 * 2. Enable writing.
 * 3. Send the Sector Erase command (0x20) and address.
 * 4. Wait until the flash finishes erasing.
 *
 * Erasing changes all bytes in the sector to 0xFF.
 */
HAL_StatusTypeDef W25Q64_EraseSector(
    W25Q64_HandleTypeDef *flash,
    uint32_t sector_address)
{
    /*
     * Sector Erase command format:
     * Byte 0: Opcode (0x20)
     * Bytes 1-3: 24-bit sector address
     */
    uint8_t tx[4] = {
        W25Q64_CMD_SECTOR_ERASE,
        0U, 0U, 0U
    };

    HAL_StatusTypeDef result;


    if (!W25Q64_IsValidHandle(flash))
    {
        return HAL_ERROR;
    }


    /*
     * The address must be inside the flash and start
     * at a 4096-byte sector boundary.
     */
    if ((sector_address >= W25Q64_CAPACITY_BYTES) ||
        ((sector_address % W25Q64_SECTOR_SIZE) != 0U))
    {
        return HAL_ERROR;
    }


    /* Set the Write Enable Latch before erasing. */
    result = W25Q64_WriteEnable(flash);

    if (result != HAL_OK)
    {
        return result;
    }


    /* Insert the three address bytes after the erase opcode. */
    W25Q64_PutAddress(
        tx,
        sector_address);


    W25Q64_Select(flash);


    /* Send the complete four-byte Sector Erase command. */
    result = HAL_SPI_Transmit(
        flash->spi,
        tx,
        sizeof(tx),
        100U);

    W25Q64_Deselect(flash);

    if (result != HAL_OK)
    {
        return result;
    }


    /*
     * Sector erasure happens internally inside the flash.
     * Wait for the BUSY bit to clear before returning.
     */
    return W25Q64_WaitReady(
        flash,
        W25Q64_ERASE_TIMEOUT_MS);
}
