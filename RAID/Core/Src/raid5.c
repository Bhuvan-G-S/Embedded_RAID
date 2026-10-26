#include "raid5.h"
#include <string.h>

static W25Q64_HandleTypeDef s_flash[RAID5_DEVICE_COUNT];
static uint8_t s_member_available[RAID5_DEVICE_COUNT];
static uint8_t s_initialized = 0U;

/* Static buffers avoid allocating several 4 KiB blocks on the stack. */
static uint8_t s_parity_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_recovery_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_verify_buffer[RAID5_BLOCK_SIZE];

static RAID5_Status RAID5_FromHAL(HAL_StatusTypeDef result)
{
    if (result == HAL_OK)
    {
        return RAID5_OK;
    }

    if (result == HAL_TIMEOUT)
    {
        return RAID5_TIMEOUT;
    }

    return RAID5_FLASH_ERROR;
}

static uint8_t RAID5_ParityDevice(uint32_t stripe_index)
{
    /* Parity rotates: stripe 0 -> device 3, stripe 1 -> device 2, etc. */
    return (uint8_t)(3U - (stripe_index % RAID5_DEVICE_COUNT));
}

static uint8_t RAID5_DataDevice(uint32_t stripe_index, uint8_t data_index)
{
    uint8_t parity_device = RAID5_ParityDevice(stripe_index);
    uint8_t seen_data_devices = 0U;
    uint8_t device;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        if (device == parity_device)
        {
            continue;
        }

        if (seen_data_devices == data_index)
        {
            return device;
        }

        seen_data_devices++;
    }

    return 0xFFU;
}

static uint32_t RAID5_PhysicalAddress(uint32_t stripe_index)
{
    return (uint32_t)RAID5_START_ADDRESS + (stripe_index * (uint32_t)RAID5_BLOCK_SIZE);
}

static uint8_t RAID5_IsInitialized(void)
{
    return s_initialized;
}

RAID5_Status RAID5_Init(W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT])
{
    uint8_t device;

    if (flashes == NULL)
    {
        return RAID5_INVALID_ARGUMENT;
    }

    s_initialized = 0U;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        uint8_t id[3] = {0U, 0U, 0U};
        HAL_StatusTypeDef result;

        s_flash[device] = flashes[device];
        s_member_available[device] = 1U;

        result = W25Q64_ReadID(&s_flash[device], id);
        if (result != HAL_OK)
        {
            return RAID5_FromHAL(result);
        }

        /* Expected W25Q64FV JEDEC ID: Winbond EF, type 40, capacity 17. */
        if ((id[0] != 0xEFU) || (id[1] != 0x40U) || (id[2] != 0x17U))
        {
            return RAID5_DEVICE_ID_ERROR;
        }
    }

    s_initialized = 1U;
    return RAID5_OK;
}

RAID5_Status RAID5_WriteStripe(
    uint32_t stripe_index,
    const uint8_t *data0,
    const uint8_t *data1,
    const uint8_t *data2
)
{
    const uint8_t *data_blocks[RAID5_DATA_BLOCKS];
    uint8_t parity_device;
    uint32_t physical_address;
    uint32_t byte_index;
    uint8_t device;
    RAID5_Status raid_result;
    HAL_StatusTypeDef flash_result;

    if (!RAID5_IsInitialized())
    {
        return RAID5_ERROR;
    }

    if ((data0 == NULL) || (data1 == NULL) || (data2 == NULL))
    {
        return RAID5_INVALID_ARGUMENT;
    }

    if (stripe_index >= RAID5_STRIPE_COUNT)
    {
        return RAID5_OUT_OF_RANGE;
    }

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        if (s_member_available[device] == 0U)
        {
            return RAID5_MEMBER_UNAVAILABLE;
        }
    }

    data_blocks[0] = data0;
    data_blocks[1] = data1;
    data_blocks[2] = data2;

    /* P = D0 XOR D1 XOR D2, one byte at a time. */
    for (byte_index = 0U; byte_index < RAID5_BLOCK_SIZE; byte_index++)
    {
        s_parity_buffer[byte_index] =
            (uint8_t)(data0[byte_index] ^ data1[byte_index] ^ data2[byte_index]);
    }

    physical_address = RAID5_PhysicalAddress(stripe_index);
    parity_device = RAID5_ParityDevice(stripe_index);

    /* Each stripe uses one 4 KiB sector on every physical chip. */
    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        flash_result = W25Q64_EraseSector(&s_flash[device], physical_address);
        raid_result = RAID5_FromHAL(flash_result);
        if (raid_result != RAID5_OK)
        {
            return raid_result;
        }
    }

    /* Program the three data blocks on their mapped devices. */
    for (device = 0U; device < RAID5_DATA_BLOCKS; device++)
    {
        uint8_t physical_device = RAID5_DataDevice(stripe_index, device);

        flash_result = W25Q64_Program(
            &s_flash[physical_device],
            physical_address,
            data_blocks[device],
            RAID5_BLOCK_SIZE
        );

        raid_result = RAID5_FromHAL(flash_result);
        if (raid_result != RAID5_OK)
        {
            return raid_result;
        }
    }

    /* Program the parity block on the rotating parity device. */
    flash_result = W25Q64_Program(
        &s_flash[parity_device],
        physical_address,
        s_parity_buffer,
        RAID5_BLOCK_SIZE
    );

    return RAID5_FromHAL(flash_result);
}

RAID5_Status RAID5_ReadBlock(uint32_t logical_block, uint8_t *buffer)
{
    uint32_t stripe_index;
    uint8_t data_index;
    uint8_t target_device;
    uint8_t parity_device;
    uint32_t physical_address;
    uint8_t other_data_index;
    HAL_StatusTypeDef flash_result;

    if (!RAID5_IsInitialized())
    {
        return RAID5_ERROR;
    }

    if (buffer == NULL)
    {
        return RAID5_INVALID_ARGUMENT;
    }

    if (logical_block >= RAID5_LOGICAL_BLOCK_COUNT)
    {
        return RAID5_OUT_OF_RANGE;
    }

    stripe_index = logical_block / RAID5_DATA_BLOCKS;
    data_index = (uint8_t)(logical_block % RAID5_DATA_BLOCKS);
    target_device = RAID5_DataDevice(stripe_index, data_index);
    parity_device = RAID5_ParityDevice(stripe_index);
    physical_address = RAID5_PhysicalAddress(stripe_index);

    /* Normal path: the requested data device is available. */
    if (s_member_available[target_device] != 0U)
    {
        flash_result = W25Q64_Read(
            &s_flash[target_device],
            physical_address,
            buffer,
            RAID5_BLOCK_SIZE
        );
        return RAID5_FromHAL(flash_result);
    }

    /* Recovery path requires parity and both other data blocks. */
    if (s_member_available[parity_device] == 0U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    flash_result = W25Q64_Read(
        &s_flash[parity_device],
        physical_address,
        buffer,
        RAID5_BLOCK_SIZE
    );
    if (flash_result != HAL_OK)
    {
        return RAID5_FromHAL(flash_result);
    }

    for (other_data_index = 0U; other_data_index < RAID5_DATA_BLOCKS; other_data_index++)
    {
        uint8_t other_device;
        uint32_t byte_index;

        if (other_data_index == data_index)
        {
            continue;
        }

        other_device = RAID5_DataDevice(stripe_index, other_data_index);
        if (s_member_available[other_device] == 0U)
        {
            return RAID5_MEMBER_UNAVAILABLE;
        }

        flash_result = W25Q64_Read(
            &s_flash[other_device],
            physical_address,
            s_recovery_buffer,
            RAID5_BLOCK_SIZE
        );
        if (flash_result != HAL_OK)
        {
            return RAID5_FromHAL(flash_result);
        }

        for (byte_index = 0U; byte_index < RAID5_BLOCK_SIZE; byte_index++)
        {
            buffer[byte_index] ^= s_recovery_buffer[byte_index];
        }
    }

    return RAID5_OK;
}

RAID5_Status RAID5_SetMemberAvailable(uint8_t member_index, uint8_t available)
{
    if (!RAID5_IsInitialized())
    {
        return RAID5_ERROR;
    }

    if (member_index >= RAID5_DEVICE_COUNT)
    {
        return RAID5_INVALID_ARGUMENT;
    }

    s_member_available[member_index] = (available != 0U) ? 1U : 0U;
    return RAID5_OK;
}

RAID5_Status RAID5_VerifyStripe(uint32_t stripe_index)
{
    uint8_t data_index;
    uint8_t parity_device;
    uint32_t physical_address;
    uint32_t byte_index;
    HAL_StatusTypeDef flash_result;
    RAID5_Status raid_result;

    if (!RAID5_IsInitialized())
    {
        return RAID5_ERROR;
    }

    if (stripe_index >= RAID5_STRIPE_COUNT)
    {
        return RAID5_OUT_OF_RANGE;
    }

    parity_device = RAID5_ParityDevice(stripe_index);
    if (s_member_available[parity_device] == 0U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    physical_address = RAID5_PhysicalAddress(stripe_index);
    memset(s_parity_buffer, 0, sizeof(s_parity_buffer));

    for (data_index = 0U; data_index < RAID5_DATA_BLOCKS; data_index++)
    {
        uint32_t logical_block = (stripe_index * RAID5_DATA_BLOCKS) + data_index;

        raid_result = RAID5_ReadBlock(logical_block, s_verify_buffer);
        if (raid_result != RAID5_OK)
        {
            return raid_result;
        }

        for (byte_index = 0U; byte_index < RAID5_BLOCK_SIZE; byte_index++)
        {
            s_parity_buffer[byte_index] ^= s_verify_buffer[byte_index];
        }
    }

    flash_result = W25Q64_Read(
        &s_flash[parity_device],
        physical_address,
        s_recovery_buffer,
        RAID5_BLOCK_SIZE
    );
    if (flash_result != HAL_OK)
    {
        return RAID5_FromHAL(flash_result);
    }

    if (memcmp(s_parity_buffer, s_recovery_buffer, RAID5_BLOCK_SIZE) != 0)
    {
        return RAID5_PARITY_MISMATCH;
    }

    return RAID5_OK;
}

const char *RAID5_StatusString(RAID5_Status status)
{
    switch (status)
    {
        case RAID5_OK:                 return "OK";
        case RAID5_INVALID_ARGUMENT:   return "invalid argument";
        case RAID5_OUT_OF_RANGE:       return "out of range";
        case RAID5_MEMBER_UNAVAILABLE: return "member unavailable";
        case RAID5_FLASH_ERROR:        return "flash I/O error";
        case RAID5_TIMEOUT:            return "timeout";
        case RAID5_DEVICE_ID_ERROR:    return "unexpected flash ID";
        case RAID5_PARITY_MISMATCH:    return "parity mismatch";
        case RAID5_ERROR:
        default:                       return "general RAID error";
    }
}
