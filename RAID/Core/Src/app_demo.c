#include "app_demo.h"

#include <stdio.h>
#include <string.h>

/* Static arrays are stored in RAM, not on the limited MCU stack. */
static uint8_t s_data0[RAID5_BLOCK_SIZE];
static uint8_t s_data1[RAID5_BLOCK_SIZE];
static uint8_t s_data2[RAID5_BLOCK_SIZE];
static uint8_t s_readback[RAID5_BLOCK_SIZE];


static void APP_SendText(
    UART_HandleTypeDef *uart,
    const char *text
)
{
    if ((uart != NULL) && (text != NULL))
    {
        HAL_UART_Transmit(
            uart,
            (uint8_t *)text,
            (uint16_t)strlen(text),
            2000U
        );
    }
}


static void APP_PrintRaidResult(
    UART_HandleTypeDef *uart,
    const char *operation,
    RAID5_Status result
)
{
    char line[128];

    (void)snprintf(
        line,
        sizeof(line),
        "%s: %s\r\n",
        operation,
        RAID5_StatusString(result)
    );

    APP_SendText(uart, line);
}


void APP_Demo_Run(
    UART_HandleTypeDef *uart,
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT]
)
{
    uint8_t device;
    uint32_t i;
    char line[128];
    RAID5_Status result;

    if ((uart == NULL) || (flashes == NULL))
    {
        return;
    }

    APP_SendText(uart, "\r\nSTM32 RAID-5 flash demo\r\n");
    APP_SendText(uart, "Checking four W25Q64FV devices...\r\n");

    /* Step 1: Check the IDs of all four flash chips. */
    for (device = 0U; device < RAID5_DEVICE_COUNT; device++)
    {
        uint8_t id[3] = {0U, 0U, 0U};

        HAL_StatusTypeDef flash_result =
            W25Q64_ReadID(&flashes[device], id);

        if (flash_result != HAL_OK)
        {
            (void)snprintf(
                line,
                sizeof(line),
                "Flash %u ID read failed (%d)\r\n",
                (unsigned int)(device + 1U),
                (int)flash_result
            );

            APP_SendText(uart, line);
            return;
        }

        (void)snprintf(
            line,
            sizeof(line),
            "Flash %u ID: %02X %02X %02X\r\n",
            (unsigned int)(device + 1U),
            (unsigned int)id[0],
            (unsigned int)id[1],
            (unsigned int)id[2]
        );

        APP_SendText(uart, line);
    }


    /* Step 2: Initialize the RAID layer. */
    result = RAID5_Init(flashes);

    APP_PrintRaidResult(uart, "RAID initialization", result);

    if (result != RAID5_OK)
    {
        return;
    }


    /* Step 3: Prepare three reproducible data patterns. */
    for (i = 0U; i < RAID5_BLOCK_SIZE; i++)
    {
        s_data0[i] = (uint8_t)(i & 0xFFU);
        s_data1[i] = (uint8_t)(((i * 3U) + 7U) & 0xFFU);
        s_data2[i] = (uint8_t)(0xA5U ^ (i & 0xFFU));
    }


    /* Step 4: Write stripe 0. */
    APP_SendText(
        uart,
        "Writing demo stripe 0 (uses physical address "
        "0x004000 on each chip)...\r\n"
    );

    APP_SendText(
        uart,
        "WARNING: stripe 0 is erased and rewritten "
        "on each MCU reset.\r\n"
    );

    result = RAID5_WriteStripe(
        0U,
        s_data0,
        s_data1,
        s_data2
    );

    APP_PrintRaidResult(uart, "Write stripe 0", result);

    if (result != RAID5_OK)
    {
        return;
    }


    /* Step 5: Read and verify logical block 0. */
    result = RAID5_ReadBlock(0U, s_readback);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data0, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(uart, "Read logical block 0: PASS\r\n");
    }
    else
    {
        APP_PrintRaidResult(uart, "Read logical block 0", result);
        APP_SendText(uart, "Readback compare block 0: FAIL\r\n");
        return;
    }


    /* Step 6: Read and verify logical block 1. */
    result = RAID5_ReadBlock(1U, s_readback);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data1, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(uart, "Read logical block 1: PASS\r\n");
    }
    else
    {
        APP_PrintRaidResult(uart, "Read logical block 1", result);
        APP_SendText(uart, "Readback compare block 1: FAIL\r\n");
        return;
    }


    /* Step 7: Read and verify logical block 2. */
    result = RAID5_ReadBlock(2U, s_readback);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data2, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(uart, "Read logical block 2: PASS\r\n");
    }
    else
    {
        APP_PrintRaidResult(uart, "Read logical block 2", result);
        APP_SendText(uart, "Readback compare block 2: FAIL\r\n");
        return;
    }


    /* Step 8: Verify the parity for stripe 0. */
    result = RAID5_VerifyStripe(0U);

    APP_PrintRaidResult(
        uart,
        "Stripe 0 parity verification",
        result
    );

    if (result != RAID5_OK)
    {
        return;
    }


    /*
     * Step 9: Simulate Flash 1 failure.
     * Flash 1 contains logical data block 0 in stripe 0.
     */
    APP_SendText(
        uart,
        "\r\nTesting Flash 1 unavailable...\r\n"
    );

    result = RAID5_SetMemberAvailable(0U, 0U);

    if (result == RAID5_OK)
    {
        result = RAID5_ReadBlock(0U, s_readback);
    }

    /* Restore Flash 1 in software. */
    (void)RAID5_SetMemberAvailable(0U, 1U);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data0, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(
            uart,
            "Flash 1 failure recovery: PASS\r\n"
        );
    }
    else
    {
        APP_PrintRaidResult(
            uart,
            "Flash 1 failure recovery",
            result
        );

        APP_SendText(
            uart,
            "Flash 1 failure recovery: FAIL\r\n"
        );
    }


    /*
     * Step 10: Simulate Flash 2 failure.
     * Flash 2 contains logical data block 1 in stripe 0.
     */
    APP_SendText(
        uart,
        "\r\nTesting Flash 2 unavailable...\r\n"
    );

    result = RAID5_SetMemberAvailable(1U, 0U);

    if (result == RAID5_OK)
    {
        result = RAID5_ReadBlock(1U, s_readback);
    }

    /* Restore Flash 2 in software. */
    (void)RAID5_SetMemberAvailable(1U, 1U);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data1, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(
            uart,
            "Flash 2 failure recovery: PASS\r\n"
        );
    }
    else
    {
        APP_PrintRaidResult(
            uart,
            "Flash 2 failure recovery",
            result
        );

        APP_SendText(
            uart,
            "Flash 2 failure recovery: FAIL\r\n"
        );
    }


    /*
     * Step 11: Simulate Flash 3 failure.
     * Flash 3 contains logical data block 2 in stripe 0.
     */
    APP_SendText(
        uart,
        "\r\nTesting Flash 3 unavailable...\r\n"
    );

    result = RAID5_SetMemberAvailable(2U, 0U);

    if (result == RAID5_OK)
    {
        result = RAID5_ReadBlock(2U, s_readback);
    }

    /* Restore Flash 3 in software. */
    (void)RAID5_SetMemberAvailable(2U, 1U);

    if ((result == RAID5_OK) &&
        (memcmp(s_readback, s_data2, RAID5_BLOCK_SIZE) == 0))
    {
        APP_SendText(
            uart,
            "Flash 3 failure recovery: PASS\r\n"
        );
    }
    else
    {
        APP_PrintRaidResult(
            uart,
            "Flash 3 failure recovery",
            result
        );

        APP_SendText(
            uart,
            "Flash 3 failure recovery: FAIL\r\n"
        );
    }


    /*
     * Step 12: Simulate Flash 4 failure.
     * Flash 4 contains parity in stripe 0.
     *
     * The three data blocks should remain readable
     * because their physical devices are available.
     */
    APP_SendText(
        uart,
        "\r\nTesting with parity member unavailable...\r\n"
    );

    result = RAID5_SetMemberAvailable(3U, 0U);

    if (result == RAID5_OK)
    {
        /* Verify block 0. */
        result = RAID5_ReadBlock(0U, s_readback);

        if ((result == RAID5_OK) &&
            (memcmp(s_readback, s_data0, RAID5_BLOCK_SIZE) == 0))
        {
            APP_SendText(
                uart,
                "Block 0 with parity unavailable: PASS\r\n"
            );
        }
        else
        {
            APP_PrintRaidResult(
                uart,
                "Block 0 read with parity unavailable",
                result
            );

            APP_SendText(
                uart,
                "Block 0 with parity unavailable: FAIL\r\n"
            );
        }


        /* Verify block 1. */
        result = RAID5_ReadBlock(1U, s_readback);

        if ((result == RAID5_OK) &&
            (memcmp(s_readback, s_data1, RAID5_BLOCK_SIZE) == 0))
        {
            APP_SendText(
                uart,
                "Block 1 with parity unavailable: PASS\r\n"
            );
        }
        else
        {
            APP_PrintRaidResult(
                uart,
                "Block 1 read with parity unavailable",
                result
            );

            APP_SendText(
                uart,
                "Block 1 with parity unavailable: FAIL\r\n"
            );
        }


        /* Verify block 2. */
        result = RAID5_ReadBlock(2U, s_readback);

        if ((result == RAID5_OK) &&
            (memcmp(s_readback, s_data2, RAID5_BLOCK_SIZE) == 0))
        {
            APP_SendText(
                uart,
                "Block 2 with parity unavailable: PASS\r\n"
            );
        }
        else
        {
            APP_PrintRaidResult(
                uart,
                "Block 2 read with parity unavailable",
                result
            );

            APP_SendText(
                uart,
                "Block 2 with parity unavailable: FAIL\r\n"
            );
        }
    }
    else
    {
        APP_PrintRaidResult(
            uart,
            "Parity-member test setup",
            result
        );
    }

    /* Restore Flash 4 in software. */
    (void)RAID5_SetMemberAvailable(3U, 1U);


    /* Step 13: Verify the stripe again after restoring all members. */
    result = RAID5_VerifyStripe(0U);

    APP_PrintRaidResult(
        uart,
        "Final stripe 0 parity verification",
        result
    );


    APP_SendText(
        uart,
        "\r\nFailure tests finished.\r\n"
    );

    APP_SendText(
        uart,
        "Demo finished. Reset to run the destructive "
        "stripe-0 demo again.\r\n"
    );
}
