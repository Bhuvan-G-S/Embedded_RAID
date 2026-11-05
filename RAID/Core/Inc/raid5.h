#ifndef RAID5_H
#define RAID5_H

#include "w25q64.h"


/*
 * RAID-5 configuration:
 * 4 physical flash chips, with 3 storing data and 1 storing parity
 * in each stripe.
 */
#define RAID5_DEVICE_COUNT         4U
#define RAID5_DATA_BLOCKS          3U


/*
 * The first four sectors of each flash chip are reserved.
 * RAID storage starts immediately after these sectors.
 */
#define RAID5_RESERVED_SECTORS     4UL

#define RAID5_START_ADDRESS \
    (RAID5_RESERVED_SECTORS * W25Q64_SECTOR_SIZE)


/* Calculate the available RAID storage space. */
#define RAID5_SECTORS_PER_DEVICE \
    (W25Q64_CAPACITY_BYTES / W25Q64_SECTOR_SIZE)

/*
 * Each sector holds one RAID stripe on each physical device.
 * Reserved sectors are excluded from the stripe count.
 */
#define RAID5_STRIPE_COUNT \
    (RAID5_SECTORS_PER_DEVICE - RAID5_RESERVED_SECTORS)

/*
 * Each stripe stores three logical data blocks.
 * The fourth physical block stores parity.
 */
#define RAID5_LOGICAL_BLOCK_COUNT \
    (RAID5_STRIPE_COUNT * RAID5_DATA_BLOCKS)

/* One RAID block is the same size as one flash sector (4 KB). */
#define RAID5_BLOCK_SIZE W25Q64_SECTOR_SIZE


/*
 * Status values returned by RAID-5 operations.
 *
 * RAID5_OK                Operation completed successfully.
 * RAID5_ERROR             General RAID error.
 * RAID5_INVALID_ARGUMENT  Invalid function argument.
 * RAID5_OUT_OF_RANGE      Requested block or stripe is out of range.
 * RAID5_MEMBER_UNAVAILABLE A required flash member is unavailable.
 * RAID5_FLASH_ERROR       Flash communication or operation failed.
 * RAID5_TIMEOUT           Flash operation timed out.
 * RAID5_DEVICE_ID_ERROR  Unexpected flash identification.
 * RAID5_PARITY_MISMATCH   Calculated parity does not match stored parity.
 */
typedef enum
{
    RAID5_OK = 0,
    RAID5_ERROR,
    RAID5_INVALID_ARGUMENT,
    RAID5_OUT_OF_RANGE,
    RAID5_MEMBER_UNAVAILABLE,
    RAID5_FLASH_ERROR,
    RAID5_TIMEOUT,
    RAID5_DEVICE_ID_ERROR,
    RAID5_PARITY_MISMATCH

} RAID5_Status;


/*
 * Initialize the RAID system with four configured flash devices.
 *
 * flashes: Array containing the SPI and chip-select configuration
 *          for each physical flash device.
 */
RAID5_Status RAID5_Init(
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT]);


/* Recheck all four physical flash chips using their JEDEC IDs. */
RAID5_Status RAID5_RefreshMembers(void);


/*
 * Check whether a physical flash member is currently available.
 *
 * member_index: Index of the flash device in the initialization array.
 *
 * Returns 1 if available, otherwise 0.
 */
uint8_t RAID5_IsMemberAvailable(uint8_t member_index);


/*
 * Check whether the RAID array is operating in degraded mode.
 *
 * Returns 1 if one or more members are unavailable, otherwise 0.
 */
uint8_t RAID5_IsDegraded(void);


/*
 * Write three data blocks and their calculated parity
 * to the physical flash devices for the specified stripe.
 *
 * All four flash devices must be available.
 *
 * Each data pointer must refer to a buffer of RAID5_BLOCK_SIZE bytes.
 */
RAID5_Status RAID5_WriteStripe(
    uint32_t stripe_index,
    const uint8_t *data0,
    const uint8_t *data1,
    const uint8_t *data2);


/*
 * Read one logical data block.
 *
 * If its physical flash member is unavailable, the missing
 * data can be reconstructed using the remaining data and parity.
 *
 * buffer: RAM buffer that receives RAID5_BLOCK_SIZE bytes.
 */
RAID5_Status RAID5_ReadBlock(
    uint32_t logical_block,
    uint8_t *buffer);


/*
 * Write one logical data block.
 *
 * The RAID layer reads the other two data blocks in the stripe,
 * recalculates parity and writes the updated stripe.
 *
 * All four flash devices must be available.
 */
RAID5_Status RAID5_WriteBlock(
    uint32_t logical_block,
    const uint8_t *data);


/*
 * Verify a stripe by recalculating its parity and comparing it
 * with the parity already stored in flash.
 *
 * Requires all four flash devices to be available.
 */
RAID5_Status RAID5_VerifyStripe(uint32_t stripe_index);


/* Convert a RAID status code into a readable text message. */
const char *RAID5_StatusString(RAID5_Status status);


#endif
