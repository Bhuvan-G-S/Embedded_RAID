#ifndef RAID5_H
#define RAID5_H

#include "w25q64.h"

#define RAID5_DEVICE_COUNT       4U
#define RAID5_DATA_BLOCKS        3U
#define RAID5_RESERVED_SECTORS   4UL
#define RAID5_START_ADDRESS     (RAID5_RESERVED_SECTORS * W25Q64_SECTOR_SIZE)
#define RAID5_SECTORS_PER_DEVICE (W25Q64_CAPACITY_BYTES / W25Q64_SECTOR_SIZE)
#define RAID5_STRIPE_COUNT      (RAID5_SECTORS_PER_DEVICE - RAID5_RESERVED_SECTORS)
#define RAID5_LOGICAL_BLOCK_COUNT (RAID5_STRIPE_COUNT * RAID5_DATA_BLOCKS)
#define RAID5_BLOCK_SIZE         W25Q64_SECTOR_SIZE

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

/* Validates all four flash IDs and saves their handles for RAID use. */
RAID5_Status RAID5_Init(W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT]);

/* Write three complete 4 KiB logical data blocks as one RAID-5 stripe. */
RAID5_Status RAID5_WriteStripe(
    uint32_t stripe_index,
    const uint8_t *data0,
    const uint8_t *data1,
    const uint8_t *data2
);

/* Read one 4 KiB logical data block; reconstruct it if its member is unavailable. */
RAID5_Status RAID5_ReadBlock(uint32_t logical_block, uint8_t *buffer);

/* Set available to 0 to simulate a failed member, or non-zero to restore it. */
RAID5_Status RAID5_SetMemberAvailable(uint8_t member_index, uint8_t available);

/* Recalculate and compare parity for one stripe. */
RAID5_Status RAID5_VerifyStripe(uint32_t stripe_index);

const char *RAID5_StatusString(RAID5_Status status);

#endif /* RAID5_H */
