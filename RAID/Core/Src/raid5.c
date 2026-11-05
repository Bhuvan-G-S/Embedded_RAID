#include "raid5.h"
#include <string.h>


/*
 * Handles for the four physical flash devices.
 *
 * These are copied from the configuration array passed to RAID5_Init().
 * The application does not need to know which physical flash stores
 * a particular logical block. This file handles that mapping.
 */
static W25Q64_HandleTypeDef s_flash[RAID5_DEVICE_COUNT];


/*
 * Tracks whether each physical flash member is currently available.
 *
 * 1 = Flash responded with the expected JEDEC ID.
 * 0 = Flash is missing or returned an unexpected ID.
 *
 * The array index identifies the physical flash:
 * 0 = Flash 1, 1 = Flash 2, 2 = Flash 3, 3 = Flash 4.
 */
static uint8_t s_member_available[RAID5_DEVICE_COUNT];


/* Set to 1 after the RAID layer has been initialized. */
static uint8_t s_initialized = 0U;


/*
 * Temporary buffers used by RAID operations.
 *
 * Static buffers keep these large 4 KB arrays out of the function
 * call stack. They are reused by the storage operations.
 *
 * s_parity_buffer   : Holds the calculated XOR parity.
 * s_recovery_buffer : Temporarily holds data read during recovery.
 * s_verify_buffer   : Holds a data block during parity verification.
 * s_write_blocks    : Holds all three data blocks during a block update.
 *
 * These buffers are shared by the RAID functions, so the current
 * implementation is intended for sequential operations, not concurrent
 * access from multiple tasks or interrupt handlers.
 */
static uint8_t s_parity_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_recovery_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_verify_buffer[RAID5_BLOCK_SIZE];
static uint8_t s_write_blocks[RAID5_DATA_BLOCKS][RAID5_BLOCK_SIZE];


/*
 * Convert an STM32 HAL result into a RAID status code.
 *
 * The flash driver returns HAL_StatusTypeDef values, whereas
 * the RAID layer uses its own RAID5_Status enumeration.
 *
 * This function keeps that conversion in one place.
 */
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

    /* Any other HAL failure is reported as a flash error. */
    return RAID5_FLASH_ERROR;
}


/*
 * Count the number of flash members currently marked available.
 *
 * The result is used to determine whether normal RAID operation
 * or single-member degraded operation is possible.
 */
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


/*
 * Determine which physical flash stores parity for a stripe.
 *
 * Parity rotates between the four flash devices:
 *
 * Stripe 0 -> Flash 4
 * Stripe 1 -> Flash 3
 * Stripe 2 -> Flash 2
 * Stripe 3 -> Flash 1
 *
 * The function returns a zero-based array index, not a flash number.
 */
static uint8_t RAID5_ParityDevice(uint32_t stripe_index)
{
    return (uint8_t)(3U - (stripe_index % RAID5_DEVICE_COUNT));
}


/*
 * Find the physical flash device that stores a particular data block
 * within a stripe.
 *
 * The parity device is skipped, and the remaining three devices
 * are assigned to data index 0, 1 and 2 in ascending device order.
 *
 * Parameters:
 * stripe_index -> Stripe whose physical layout is being checked.
 * data_index   -> Position of the data block within that stripe.
 *
 * Returns:
 * Zero-based physical flash index.
 * 0xFF if the requested data index is invalid.
 */
static uint8_t RAID5_DataDevice(
    uint32_t stripe_index,
    uint8_t data_index)
{
    uint8_t parity_device = RAID5_ParityDevice(stripe_index);
    uint8_t data_count = 0U;
    uint8_t device;

    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        /* The parity device does not store a data block. */
        if (device == parity_device)
        {
            continue;
        }

        /*
         * Count only the devices that store data.
         * Return the device when the requested data position is reached.
         */
        if (data_count == data_index)
        {
            return device;
        }

        data_count++;
    }

    return 0xFFU;
}


/*
 * Convert a stripe index into its physical address on a flash device.
 *
 * All four chips use the same physical address for a given stripe.
 * Their contents differ depending on whether they store data or parity.
 *
 * The first four sectors are reserved, so RAID data begins at 0x4000.
 *
 * Physical address = RAID5_START_ADDRESS
 *                  + (stripe_index * RAID5_BLOCK_SIZE)
 */
static uint32_t RAID5_PhysicalAddress(uint32_t stripe_index)
{
    return (uint32_t)RAID5_START_ADDRESS +
           (stripe_index * (uint32_t)RAID5_BLOCK_SIZE);
}


/*
 * Return the current availability state of a physical flash member.
 *
 * member_index is zero-based:
 * 0 = Flash 1
 * 1 = Flash 2
 * 2 = Flash 3
 * 3 = Flash 4
 *
 * Returns 1 if available, otherwise 0.
 */
uint8_t RAID5_IsMemberAvailable(uint8_t member_index)
{
    if ((!s_initialized) ||
        (member_index >= RAID5_DEVICE_COUNT))
    {
        return 0U;
    }

    return s_member_available[member_index];
}


/*
 * Check whether the RAID array is operating in degraded mode.
 *
 * Degraded mode means at least one of the four members is unavailable.
 * With three available members, RAID-5 can still reconstruct missing data.
 *
 * Returns:
 * 1 = Degraded or not initialized.
 * 0 = All four members available.
 */
uint8_t RAID5_IsDegraded(void)
{
    if (!s_initialized)
    {
        return 1U;
    }

    return (RAID5_CountAvailable() < RAID5_DEVICE_COUNT) ? 1U : 0U;
}


/*
 * Recheck the physical availability of all four flash devices.
 *
 * Each flash is identified by reading its JEDEC ID.
 * The expected W25Q64FV ID is EF 40 17.
 *
 * HAL_OK alone does not prove that the correct flash responded.
 * The returned ID must also match the expected manufacturer,
 * memory type and capacity bytes.
 *
 * This function updates s_member_available[] every time it runs.
 *
 * Returns:
 * RAID5_OK                -> Three or four valid members detected.
 * RAID5_MEMBER_UNAVAILABLE -> Fewer than three valid members.
 * RAID5_ERROR             -> RAID layer has not been initialized.
 */
RAID5_Status RAID5_RefreshMembers(void)
{
    uint8_t device;

    if (!s_initialized)
    {
        return RAID5_ERROR;
    }

    /* Probe each physical flash independently. */
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

    /*
     * RAID-5 can tolerate one unavailable member.
     * If fewer than three members respond, normal recovery is impossible.
     */
    if (RAID5_CountAvailable() < 3U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    return RAID5_OK;
}


/*
 * Initialize the RAID-5 layer.
 *
 * This function:
 * 1. Stores the configuration of all four flash devices.
 * 2. Clears their initial availability states.
 * 3. Probes the devices using RAID5_RefreshMembers().
 * 4. Allows initialization with three or four valid members.
 *
 * No sectors are erased or programmed during initialization.
 */
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

    /* Copy each flash configuration into the RAID layer. */
    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        s_flash[device] = flashes[device];
        s_member_available[device] = 0U;
    }

    /*
     * Enable the member-detection function.
     * It checks s_initialized before performing the probe.
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


/*
 * Write three logical data blocks as one RAID-5 stripe.
 *
 * A stripe contains:
 * - Three data blocks.
 * - One XOR parity block.
 *
 * The parity position rotates between the four flash devices.
 *
 * The operation:
 * 1. Validates the request.
 * 2. Checks that all four members are available.
 * 3. Calculates XOR parity.
 * 4. Erases the corresponding sector on all four chips.
 * 5. Programs the three data blocks.
 * 6. Programs the parity block.
 *
 * All four members are required because this function performs
 * destructive erase and program operations.
 *
 * Note: A power loss or hardware failure during this sequence
 * can leave the stripe partially updated. This implementation
 * does not provide transaction or power-loss protection.
 */
RAID5_Status RAID5_WriteStripe(
    uint32_t stripe_index,
    const uint8_t *data0,
    const uint8_t *data1,
    const uint8_t *data2)
{
    /* Pointers to the three input data blocks. */
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

    /* Refresh detection before performing any destructive operation. */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }

    /* A complete stripe write requires all four physical members. */
    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }

    /* Store the addresses of the three input buffers. */
    data_blocks[0] = data0;
    data_blocks[1] = data1;
    data_blocks[2] = data2;


    /*
     * Calculate parity byte by byte.
     *
     * P = D0 XOR D1 XOR D2
     *
     * Each byte of the parity block is calculated from
     * the corresponding byte of all three data blocks.
     */
    for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
    {
        s_parity_buffer[i] =
            (uint8_t)(data0[i] ^ data1[i] ^ data2[i]);
    }


    /* Find the common physical address and parity device for this stripe. */
    address = RAID5_PhysicalAddress(stripe_index);
    parity_device = RAID5_ParityDevice(stripe_index);


    /*
     * Erase the stripe's 4 KB sector on every flash.
     *
     * All chips use the same physical address for this stripe.
     * Erasing is required before programming new data into NOR flash.
     */
    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        flash_result = W25Q64_EraseSector(
            &s_flash[device],
            address);

        raid_result = RAID5_FromHAL(flash_result);

        if (raid_result != RAID5_OK)
        {
            /* Mark the member unavailable if the erase fails. */
            s_member_available[device] = 0U;
            return raid_result;
        }
    }


    /*
     * Program the three data blocks.
     *
     * RAID5_DataDevice() determines which physical chip stores
     * each data block for the selected stripe.
     */
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


    /* Store the calculated parity block on its rotating parity member. */
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


/*
 * Read one logical block from the RAID-5 array.
 *
 * The logical block number is converted into:
 * - A stripe index.
 * - A data position within that stripe.
 *
 * If the required data member is online, the block is read directly.
 *
 * If that member is unavailable, the missing data is reconstructed:
 *
 * Missing data = Parity XOR Other data block 1 XOR Other data block 2
 *
 * The reconstructed result is returned in the supplied RAM buffer.
 * The reconstructed data is not automatically written back to flash.
 */
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

    /*
     * Check physical member availability before deciding
     * whether to perform a normal read or reconstruction.
     */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }


    /*
     * Convert the logical block number into its stripe and
     * position inside the stripe.
     *
     * Example:
     * Logical block 5:
     * Stripe index = 5 / 3 = 1
     * Data index   = 5 % 3 = 2
     */
    stripe_index = logical_block / RAID5_DATA_BLOCKS;
    data_index = (uint8_t)(logical_block % RAID5_DATA_BLOCKS);


    /* Determine which physical devices hold this block and its parity. */
    target_device = RAID5_DataDevice(stripe_index, data_index);
    parity_device = RAID5_ParityDevice(stripe_index);

    /* All members use the same address for this stripe. */
    address = RAID5_PhysicalAddress(stripe_index);


    /*
     * NORMAL READ
     *
     * If the physical device containing the requested data is online,
     * read the data directly into the caller's buffer.
     */
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


    /*
     * RECOVERY CHECK
     *
     * Reconstruction is possible only when at least three
     * members are still available.
     */
    if (RAID5_CountAvailable() < (RAID5_DEVICE_COUNT - 1U))
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }


    /* The parity block must be available for reconstruction. */
    if (s_member_available[parity_device] == 0U)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }


    /*
     * Read the stored parity block into the output buffer.
     *
     * The output buffer will gradually become the reconstructed
     * data block as we XOR the surviving data blocks into it.
     */
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
     * XOR the two surviving data blocks with parity.
     *
     * Skip the data position that is missing.
     *
     * After both surviving data blocks have been processed:
     *
     * buffer = P XOR D1 XOR D2
     *
     * The result is the missing data block.
     */
    for (other_index = 0U;
         other_index < RAID5_DATA_BLOCKS;
         other_index++)
    {
        uint8_t other_device;
        uint32_t i;

        /* Do not read the data position we are reconstructing. */
        if (other_index == data_index)
        {
            continue;
        }

        /* Find the physical device containing this surviving block. */
        other_device = RAID5_DataDevice(stripe_index, other_index);

        if (s_member_available[other_device] == 0U)
        {
            return RAID5_MEMBER_UNAVAILABLE;
        }

        /* Read the surviving data block into a temporary buffer. */
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

        /*
         * XOR the surviving block into the parity data.
         * Repeat for all 4096 bytes.
         */
        for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
        {
            buffer[i] ^= s_recovery_buffer[i];
        }
    }

    return RAID5_OK;
}


/*
 * Write a single logical data block while preserving the other
 * two data blocks in its stripe.
 *
 * RAID-5 parity depends on all three data blocks. Therefore,
 * updating one logical block requires the other two blocks
 * to be read before the new parity can be calculated.
 *
 * This is called read-modify-write:
 *
 * 1. Read the two unchanged data blocks.
 * 2. Copy the new data into the requested data position.
 * 3. Rewrite the complete stripe and regenerate parity.
 *
 * All four flash members must be available for this operation.
 */
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


    /* Refresh member detection before allowing a write. */
    result = RAID5_RefreshMembers();

    if (result != RAID5_OK)
    {
        return result;
    }


    /* Do not perform read-modify-write with a missing member. */
    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }


    /* Find the stripe and data position for the requested block. */
    stripe_index = logical_block / RAID5_DATA_BLOCKS;
    target_index = (uint8_t)(logical_block % RAID5_DATA_BLOCKS);


    /*
     * Preserve the two existing data blocks.
     *
     * The target data position is skipped because its old contents
     * will be replaced with the new data supplied by the application.
     */
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


    /* Replace the requested data block with the new application data. */
    memcpy(
        s_write_blocks[target_index],
        data,
        RAID5_BLOCK_SIZE);


    /*
     * Write all three data blocks again.
     * RAID5_WriteStripe() recalculates and stores the updated parity.
     */
    return RAID5_WriteStripe(
        stripe_index,
        s_write_blocks[0],
        s_write_blocks[1],
        s_write_blocks[2]);
}


/*
 * Verify the parity of one stripe.
 *
 * This function reads the three stored data blocks, recalculates
 * their XOR parity and compares it with the parity block stored
 * on the corresponding physical flash.
 *
 * All four flash members must be available because this operation
 * compares the original stored data and stored parity directly.
 *
 * Returns RAID5_PARITY_MISMATCH if the calculated and stored
 * parity blocks are different.
 */
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


    /* Refresh member availability before accessing the flash devices. */
    raid_result = RAID5_RefreshMembers();

    if (raid_result != RAID5_OK)
    {
        return raid_result;
    }


    /* Parity verification requires all four physical members. */
    if (RAID5_CountAvailable() != RAID5_DEVICE_COUNT)
    {
        return RAID5_MEMBER_UNAVAILABLE;
    }


    parity_device = RAID5_ParityDevice(stripe_index);
    address = RAID5_PhysicalAddress(stripe_index);


    /* Clear the parity buffer before recalculating XOR parity. */
    memset(s_parity_buffer, 0, sizeof(s_parity_buffer));


    /*
     * Read each physical data block and XOR it into s_parity_buffer.
     *
     * After all three blocks have been processed, this buffer
     * contains the parity calculated from the stored data.
     */
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

        /* Combine the current data block with the calculated parity. */
        for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
        {
            s_parity_buffer[i] ^= s_verify_buffer[i];
        }
    }


    /* Read the parity block that is actually stored on the flash. */
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


    /*
     * Compare calculated parity with stored parity.
     *
     * memcmp() compares the two buffers byte by byte.
     * A result of zero means the buffers are identical.
     */
    if (memcmp(
            s_parity_buffer,
            s_recovery_buffer,
            RAID5_BLOCK_SIZE) != 0)
    {
        return RAID5_PARITY_MISMATCH;
    }

    return RAID5_OK;
}


/*
 * Convert a RAID status code into a readable message.
 *
 * This is used by the UART console to display operation results
 * without needing to understand the numeric status values.
 */
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
