#include "raid5.h"
#include <string.h>

static W25Q64_HandleTypeDef s_flash[RAID5_DEVICE_COUNT];
static uint8_t s_member_available[RAID5_DEVICE_COUNT];
static uint8_t s_initialized = 0U;

/* Static buffers keep large arrays off the stack. */
static uint8_t s_parity_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_recovery_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_verify_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_write_blocks[RAID5_DATA_BLOCKS][RAID5_BLOCK_SIZE];

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

static uint8_t RAID5_CountAvailable(void)
{
    uint8_t device;
    uint8_t count = 0U;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        if (s_member_available[device] != 0U)
        {
            count++;
        }
    }

    return count;
}

static uint8_t RAID5_ParityDevice(uint32_t stripe_index)
{
    /* Stripe 0 -> Flash 4; stripe 1 -> Flash 3; stripe 2 -> Flash 2. */
    return (uint8_t)(3U - (stripe_index % RAID5_DEVICE_COUNT));
}

static uint8_t RAID5_DataDevice(
    uint32_t stripe_index,
    uint8_t data_index)
{
    uint8_t parity_device = RAID5_ParityDevice(stripe_index);
    uint8_t data_count = 0U;
    uint8_t device;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        if (device == parity_device)
        {
            continue;
        }

        if (data_count == data_index)
        {
            return device;
        }

        data_count++;
    }

    return 0xFFU;
}

static uint32_t RAID5_PhysicalAddress(uint32_t stripe_index)
{
    return (uint32_t)RAID5_START_ADDRESS +
           (stripe_index * (uint32_t)RAID5_BLOCK_SIZE);
}

uint8_t RAID5_IsMemberAvailable(uint8_t member_index)
{
    if ((!s_initialized) ||
        (member_index >= RAID5_DEVICE_COUNT))
    {
        return 0U;
    }

    return s_member_available[member_index];
}

uint8_t RAID5_IsDegraded(void)
{
    if (!s_initialized)
    {
        return 1U;
    }

    return (RAID5_CountAvailable() < RAID5_DEVICE_COUNT) ? 1U : 0U;
}

/*
 * Probe every chip by reading its JEDEC ID.
 * HAL_OK by itself is not sufficient: the ID must match EF 40 17.
 */
RAID5_Status RAID5_RefreshMembers(void)
{
    uint8_t device;

    if (!s_initialized)
    {
        return RAID5_ERROR;
    }

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        uint8_t id[3] = {0U, 0U, 0U};

        HAL_StatusTypeDef result =
            W25Q64_ReadID(&s_flash[device], id);

        if ((result == HAL_OK) &&
            (id[0] == 0xEFU) &&
            (id[1] == 0x40U) &&
            (id[2] == 0x17U))
        {
            s_member_available[device] = 1U;
        }
        else
        {
            s_member_available[device] = 0U;
        }
    }

    if (RAID5_CountAvailable() < 3U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    return RAID5_OK;
}

RAID5_Status RAID5_Init(
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT])
{
    uint8_t device;
    RAID5_Status result;

    if (flashes == NULL)
    {
        return RAID5_INVALID_ARGUMENT;
    }

    s_initialized = 0U;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        s_flash[device] = flashes[device];
        s_member_available[device] = 0U;
    }

    /*
     * Enable probing, then refresh member detection.
     * Initialization accepts four members or one missing member.
     */
    s_initialized = 1U;

    result = RAID5_RefreshMembers();

    if (result != RAID5_OK)
    {
        s_initialized = 0U;
        return result;
    }

    return RAID5_OK;
}

RAID5_Status RAID5_WriteStripe(
    uint32_t stripe_index,
    const uint8_t *data0,
    const uint8_t *data1,
    const uint8_t *data2)
{
    const uint8_t *data_blocks[RAID5_DATA_BLOCKS];

    uint8_t device;
    uint8_t parity_device;
    uint32_t address;
    uint32_t i;

    RAID5_Status raid_result;
    HAL_StatusTypeDef flash_result;

    if (!s_initialized)
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

    /* Refresh detection before any destructive operation. */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }

    /* All four members are required for stripe writes. */
    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    data_blocks[0] = data0;
    data_blocks[1] = data1;
    data_blocks[2] = data2;

    /* Calculate P = D0 XOR D1 XOR D2. */
    for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
    {
        s_parity_buffer[i] =
            (uint8_t)(data0[i] ^ data1[i] ^ data2[i]);
    }

    address = RAID5_PhysicalAddress(stripe_index);
    parity_device = RAID5_ParityDevice(stripe_index);

    /* Erase the stripe's sector on all four chips. */
    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        flash_result = W25Q64_EraseSector(
            &s_flash[device],
            address);

        raid_result = RAID5_FromHAL(flash_result);

        if (raid_result != RAID5_OK)
        {
            s_member_available[device] = 0U;
            return raid_result;
        }
    }

    /* Write the three data blocks. */
    for (device = 0U; device < RAID5_DATA_BLOCKS; device++)
    {
        uint8_t physical_device =
            RAID5_DataDevice(stripe_index, device);

        flash_result = W25Q64_Program(
            &s_flash[physical_device],
            address,
            data_blocks[device],
            RAID5_BLOCK_SIZE);

        raid_result = RAID5_FromHAL(flash_result);

        if (raid_result != RAID5_OK)
        {
            s_member_available[physical_device] = 0U;
            return raid_result;
        }
    }

    /* Write parity to the rotating parity member. */
    flash_result = W25Q64_Program(
        &s_flash[parity_device],
        address,
        s_parity_buffer,
        RAID5_BLOCK_SIZE);

    raid_result = RAID5_FromHAL(flash_result);

    if (raid_result != RAID5_OK)
    {
        s_member_available[parity_device] = 0U;
    }

    return raid_result;
}

RAID5_Status RAID5_ReadBlock(
    uint32_t logical_block,
    uint8_t *buffer)
{
    uint32_t stripe_index;
    uint8_t data_index;
    uint8_t target_device;
    uint8_t parity_device;
    uint8_t other_index;

    uint32_t address;

    HAL_StatusTypeDef flash_result;
    RAID5_Status raid_result;

    if (!s_initialized)
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

    /* Refresh physical availability before deciding the read path. */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }

    stripe_index = logical_block / RAID5_DATA_BLOCKS;
    data_index = (uint8_t)(logical_block % RAID5_DATA_BLOCKS);

    target_device = RAID5_DataDevice(stripe_index, data_index);
    parity_device = RAID5_ParityDevice(stripe_index);
    address = RAID5_PhysicalAddress(stripe_index);

    /* Normal read if the required data member is online. */
    if (s_member_available[target_device] != 0U)
    {
        flash_result = W25Q64_Read(
            &s_flash[target_device],
            address,
            buffer,
            RAID5_BLOCK_SIZE);

        if (flash_result == HAL_OK)
        {
            return RAID5_OK;
        }

        /* A failed data read marks this member unavailable. */
        s_member_available[target_device] = 0U;
    }

    if (RAID5_CountAvailable() < (RAID5_DEVICE_COUNT - 1U))
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    /* Reconstruction requires the parity member. */
    if (s_member_available[parity_device] == 0U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    flash_result = W25Q64_Read(
        &s_flash[parity_device],
        address,
        buffer,
        RAID5_BLOCK_SIZE);

    if (flash_result != HAL_OK)
    {
        s_member_available[parity_device] = 0U;
        return RAID5_MEMBER_UNAVAILABLE;
    }

    /*
     * Missing data = parity XOR the other two data blocks.
     */
    for (other_index = 0U;
         other_index < RAID5_DATA_BLOCKS;
         other_index++)
    {
        uint8_t other_device;
        uint32_t i;

        if (other_index == data_index)
        {
            continue;
        }

        other_device = RAID5_DataDevice(stripe_index, other_index);

        if (s_member_available[other_device] == 0U)
        {
            return RAID5_MEMBER_UNAVAILABLE;
        }

        flash_result = W25Q64_Read(
            &s_flash[other_device],
            address,
            s_recovery_buffer,
            RAID5_BLOCK_SIZE);

        if (flash_result != HAL_OK)
        {
            s_member_available[other_device] = 0U;
            return RAID5_MEMBER_UNAVAILABLE;
        }

        for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
        {
            buffer[i] ^= s_recovery_buffer[i];
        }
    }

    return RAID5_OK;
}

RAID5_Status RAID5_WriteBlock(
    uint32_t logical_block,
    const uint8_t *data)
{
    uint32_t stripe_index;
    uint8_t target_index;
    uint8_t data_index;

    RAID5_Status result;

    if (!s_initialized)
    {
        return RAID5_ERROR;
    }

    if (data == NULL)
    {
        return RAID5_INVALID_ARGUMENT;
    }

    if (logical_block >= RAID5_LOGICAL_BLOCK_COUNT)
    {
        return RAID5_OUT_OF_RANGE;
    }

    /* Reject writes when any member is missing. */
    result = RAID5_RefreshMembers();

    if (result != RAID5_OK)
    {
        return result;
    }

    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    stripe_index = logical_block / RAID5_DATA_BLOCKS;
    target_index = (uint8_t)(logical_block % RAID5_DATA_BLOCKS);

    /* Preserve the two other blocks in the same stripe. */
    for (data_index = 0U;
         data_index < RAID5_DATA_BLOCKS;
         data_index++)
    {
        uint32_t current_logical_block =
            (stripe_index * RAID5_DATA_BLOCKS) + data_index;

        if (data_index == target_index)
        {
            continue;
        }

        result = RAID5_ReadBlock(
            current_logical_block,
            s_write_blocks[data_index]);

        if (result != RAID5_OK)
        {
            return result;
        }
    }

    memcpy(
        s_write_blocks[target_index],
        data,
        RAID5_BLOCK_SIZE);

    /* Rewrite the complete stripe and regenerate parity. */
    return RAID5_WriteStripe(
        stripe_index,
        s_write_blocks[0],
        s_write_blocks[1],
        s_write_blocks[2]);
}

RAID5_Status RAID5_VerifyStripe(uint32_t stripe_index)
{
    uint8_t data_index;
    uint8_t parity_device;

    uint32_t address;
    uint32_t i;

    HAL_StatusTypeDef flash_result;
    RAID5_Status raid_result;

    if (!s_initialized)
    {
        return RAID5_ERROR;
    }

    if (stripe_index >= RAID5_STRIPE_COUNT)
    {
        return RAID5_OUT_OF_RANGE;
    }

    /* Refresh before verifying physical data and parity. */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }

    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    parity_device = RAID5_ParityDevice(stripe_index);
    address = RAID5_PhysicalAddress(stripe_index);

    memset(s_parity_buffer, 0, sizeof(s_parity_buffer));

    /* Recalculate parity directly from the three stored data blocks. */
    for (data_index = 0U;
         data_index < RAID5_DATA_BLOCKS;
         data_index++)
    {
        uint8_t physical_device =
            RAID5_DataDevice(stripe_index, data_index);

        flash_result = W25Q64_Read(
            &s_flash[physical_device],
            address,
            s_verify_buffer,
            RAID5_BLOCK_SIZE);

        if (flash_result != HAL_OK)
        {
            s_member_available[physical_device] = 0U;
            return RAID5_FromHAL(flash_result);
        }

        for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
        {
            s_parity_buffer[i] ^= s_verify_buffer[i];
        }
    }

    flash_result = W25Q64_Read(
        &s_flash[parity_device],
        address,
        s_recovery_buffer,
        RAID5_BLOCK_SIZE);

    if (flash_result != HAL_OK)
    {
        s_member_available[parity_device] = 0U;
        return RAID5_FromHAL(flash_result);
    }

    if (memcmp(
            s_parity_buffer,
            s_recovery_buffer,
            RAID5_BLOCK_SIZE) != 0)
    {
        return RAID5_PARITY_MISMATCH;
    }

    return RAID5_OK;
}

const char *RAID5_StatusString(RAID5_Status status)
{
    switch (status)
    {
        case RAID5_OK:
            return "OK";

        case RAID5_INVALID_ARGUMENT:
            return "invalid argument";

        case RAID5_OUT_OF_RANGE:
            return "out of range";

        case RAID5_MEMBER_UNAVAILABLE:
            return "member unavailable";

        case RAID5_FLASH_ERROR:
            return "flash I/O error";

        case RAID5_TIMEOUT:
            return "timeout";

        case RAID5_DEVICE_ID_ERROR:
            return "unexpected flash ID";

        case RAID5_PARITY_MISMATCH:
            return "parity mismatch";

        case RAID5_ERROR:
        default:
            return "general RAID error";
    }
}
