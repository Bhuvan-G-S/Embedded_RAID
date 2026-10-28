#include "app_demo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define APP_LINE_SIZE       160U
#define APP_MAX_TEXT_LENGTH 120U
#define APP_RECORD_HEADER   6U

static uint8_t s_cli_block[RAID5_BLOCK_SIZE];
static uint8_t s_ignore_lf = 0U;

static const uint8_t s_text_magic[4] = {
    'T', 'X', 'T', '1'
};

static void APP_SendText(
    UART_HandleTypeDef *uart,
    const char *text)
{
    if ((uart != NULL) && (text != NULL))
    {
        HAL_UART_Transmit(
            uart,
            (uint8_t *)text,
            (uint16_t)strlen(text),
            HAL_MAX_DELAY);
    }
}

static void APP_PrintResult(
    UART_HandleTypeDef *uart,
    const char *operation,
    RAID5_Status result)
{
    char line[128];

    (void)snprintf(
        line,
        sizeof(line),
        "%s: %s\r\n",
        operation,
        RAID5_StatusString(result));

    APP_SendText(uart, line);
}

static uint8_t APP_ReadLine(
    UART_HandleTypeDef *uart,
    char *line,
    uint16_t capacity)
{
    uint16_t length = 0U;

    if ((uart == NULL) || (line == NULL) || (capacity < 2U))
    {
        return 0U;
    }

    while (1)
    {
        uint8_t ch;

        if (HAL_UART_Receive(
                uart,
                &ch,
                1U,
                HAL_MAX_DELAY) != HAL_OK)
        {
            continue;
        }

        if (s_ignore_lf != 0U)
        {
            s_ignore_lf = 0U;

            if (ch == '\n')
            {
                continue;
            }
        }

        if (ch == '\r')
        {
            s_ignore_lf = 1U;
            line[length] = '\0';
            APP_SendText(uart, "\r\n");
            return 1U;
        }

        if (ch == '\n')
        {
            line[length] = '\0';
            APP_SendText(uart, "\r\n");
            return 1U;
        }

        if ((ch == '\b') || (ch == 0x7FU))
        {
            if (length > 0U)
            {
                length--;
                APP_SendText(uart, "\b \b");
            }

            continue;
        }

        if ((ch >= 32U) && (ch <= 126U) &&
            (length < (capacity - 1U)))
        {
            line[length++] = (char)ch;

            (void)HAL_UART_Transmit(
                uart,
                &ch,
                1U,
                HAL_MAX_DELAY);
        }
    }
}

static uint8_t APP_ParseBlockNumber(
    const char *text,
    uint32_t *block,
    const char **end_position)
{
    char *end;
    unsigned long value;

    if ((text == NULL) ||
        (block == NULL) ||
        (text[0] < '0') ||
        (text[0] > '9'))
    {
        return 0U;
    }

    value = strtoul(text, &end, 10);

    if (end == text)
    {
        return 0U;
    }

    if (value >= RAID5_LOGICAL_BLOCK_COUNT)
    {
        return 0U;
    }

    *block = (uint32_t)value;

    if (end_position != NULL)
    {
        *end_position = end;
    }

    return 1U;
}

/*
 * Refresh physical member detection every time status is requested.
 * This is the fix for showing a disconnected flash as ONLINE.
 */
static void APP_PrintStatus(UART_HandleTypeDef *uart)
{
    uint8_t device;
    uint8_t online = 0U;

    char line[128];

    RAID5_Status result = RAID5_RefreshMembers();

    if (result == RAID5_MEMBER_UNAVAILABLE)
    {
        APP_SendText(
            uart,
            "WARNING: fewer than 3 valid members detected!\r\n");
    }
    else if (result != RAID5_OK)
    {
        APP_PrintResult(uart, "Member detection", result);
    }

    APP_SendText(uart, "\r\nStorage status\r\n");

    for (device = 0U;
         device < RAID5_DEVICE_COUNT;
         device++)
    {
        uint8_t available =
            RAID5_IsMemberAvailable(device);

        (void)snprintf(
            line,
            sizeof(line),
            "Flash %u: %s\r\n",
            (unsigned int)(device + 1U),
            (available != 0U) ? "ONLINE" : "OFFLINE");

        APP_SendText(uart, line);

        if (available != 0U)
        {
            online++;
        }
    }

    (void)snprintf(
        line,
        sizeof(line),
        "Members online: %u/4\r\n"
        "Logical blocks: %lu\r\n"
        "Block size: %lu bytes\r\n"
        "Mode: %s\r\n",
        (unsigned int)online,
        (unsigned long)RAID5_LOGICAL_BLOCK_COUNT,
        (unsigned long)RAID5_BLOCK_SIZE,
        (RAID5_IsDegraded() != 0U)
            ? "DEGRADED - READ ONLY"
            : "RAID-5 - READ/WRITE");

    APP_SendText(uart, line);
}

static void APP_ReadCommand(
    UART_HandleTypeDef *uart,
    uint32_t logical_block)
{
    RAID5_Status result;
    uint16_t text_length;
    char line[100];

    result = RAID5_ReadBlock(
        logical_block,
        s_cli_block);

    if (result != RAID5_OK)
    {
        APP_PrintResult(uart, "Read", result);
        return;
    }

    (void)snprintf(
        line,
        sizeof(line),
        "Logical block %lu\r\n",
        (unsigned long)logical_block);

    APP_SendText(uart, line);

    if (memcmp(
            s_cli_block,
            s_text_magic,
            sizeof(s_text_magic)) != 0)
    {
        APP_SendText(
            uart,
            "No console text record in this block "
            "(unwritten or older/raw data).\r\n");

        return;
    }

    text_length =
        (uint16_t)s_cli_block[4] |
        ((uint16_t)s_cli_block[5] << 8);

    if (text_length >
        (RAID5_BLOCK_SIZE - APP_RECORD_HEADER))
    {
        APP_SendText(uart, "Invalid text record length.\r\n");
        return;
    }

    APP_SendText(uart, "Data: ");

    if (text_length > 0U)
    {
        (void)HAL_UART_Transmit(
            uart,
            &s_cli_block[APP_RECORD_HEADER],
            text_length,
            HAL_MAX_DELAY);
    }

    APP_SendText(uart, "\r\n");
}

static void APP_WriteCommand(
    UART_HandleTypeDef *uart,
    uint32_t logical_block,
    const char *text)
{
    size_t text_length;
    RAID5_Status result;

    if (text == NULL)
    {
        APP_SendText(uart, "Text is missing.\r\n");
        return;
    }

    text_length = strlen(text);

    if (text_length == 0U)
    {
        APP_SendText(uart, "Text cannot be empty.\r\n");
        return;
    }

    if (text_length > APP_MAX_TEXT_LENGTH)
    {
        APP_SendText(
            uart,
            "Text is too long. Maximum is 120 characters.\r\n");

        return;
    }

    memset(s_cli_block, 0xFF, sizeof(s_cli_block));

    memcpy(
        s_cli_block,
        s_text_magic,
        sizeof(s_text_magic));

    s_cli_block[4] = (uint8_t)(text_length & 0xFFU);
    s_cli_block[5] = (uint8_t)((text_length >> 8) & 0xFFU);

    memcpy(
        &s_cli_block[APP_RECORD_HEADER],
        text,
        text_length);

    result = RAID5_WriteBlock(
        logical_block,
        s_cli_block);

    APP_PrintResult(uart, "Write", result);
}

static void APP_VerifyCommand(
    UART_HandleTypeDef *uart,
    uint32_t logical_block)
{
    uint32_t stripe_index =
        logical_block / RAID5_DATA_BLOCKS;

    RAID5_Status result;
    char line[100];

    (void)snprintf(
        line,
        sizeof(line),
        "Verifying stripe %lu...\r\n",
        (unsigned long)stripe_index);

    APP_SendText(uart, line);

    result = RAID5_VerifyStripe(stripe_index);

    APP_PrintResult(uart, "Parity verification", result);
}

static void APP_PrintHelp(UART_HandleTypeDef *uart)
{
    char line[64];

    APP_SendText(
        uart,
        "\r\nCommands:\r\n"
        "  help                   Show commands\r\n"
        "  status                 Show RAID member status\r\n"
        "  read <block>           Read a logical block\r\n"
        "  write <block> <text>   Write text to a logical block\r\n"
        "  verify <block>         Verify parity for its stripe\r\n"
        "\r\n"
        "Logical block range: 0 to ");

    (void)snprintf(
        line,
        sizeof(line),
        "%lu\r\n",
        (unsigned long)(RAID5_LOGICAL_BLOCK_COUNT - 1UL));

    APP_SendText(uart, line);

    APP_SendText(
        uart,
        "Text writes accept up to 120 characters.\r\n"
        "Writes require all four flash members online.\r\n"
        "Normal startup does not erase storage.\r\n");
}

static void APP_ProcessCommand(
    UART_HandleTypeDef *uart,
    char *line)
{
    char *command = line;

    while (*command == ' ')
    {
        command++;
    }

    {
        size_t length = strlen(command);

        while ((length > 0U) &&
               (command[length - 1U] == ' '))
        {
            command[length - 1U] = '\0';
            length--;
        }
    }

    if (command[0] == '\0')
    {
        return;
    }

    if (strcmp(command, "help") == 0)
    {
        APP_PrintHelp(uart);
        return;
    }

    if (strcmp(command, "status") == 0)
    {
        APP_PrintStatus(uart);
        return;
    }

    if (strncmp(command, "read ", 5U) == 0)
    {
        uint32_t block;
        const char *end_position;

        if (!APP_ParseBlockNumber(
                command + 5U,
                &block,
                &end_position) ||
            (*end_position != '\0'))
        {
            APP_SendText(
                uart,
                "Usage: read <logical_block>\r\n");
            return;
        }

        APP_ReadCommand(uart, block);
        return;
    }

    if (strncmp(command, "write ", 6U) == 0)
    {
        uint32_t block;
        const char *end_position;
        const char *text;

        if (!APP_ParseBlockNumber(
                command + 6U,
                &block,
                &end_position))
        {
            APP_SendText(
                uart,
                "Usage: write <logical_block> <text>\r\n");
            return;
        }

        if (*end_position != ' ')
        {
            APP_SendText(
                uart,
                "Usage: write <logical_block> <text>\r\n");
            return;
        }

        text = end_position;

        while (*text == ' ')
        {
            text++;
        }

        if (*text == '\0')
        {
            APP_SendText(
                uart,
                "Enter text after the block number.\r\n");
            return;
        }

        APP_WriteCommand(uart, block, text);
        return;
    }

    if (strncmp(command, "verify ", 7U) == 0)
    {
        uint32_t block;
        const char *end_position;

        if (!APP_ParseBlockNumber(
                command + 7U,
                &block,
                &end_position) ||
            (*end_position != '\0'))
        {
            APP_SendText(
                uart,
                "Usage: verify <logical_block>\r\n");
            return;
        }

        APP_VerifyCommand(uart, block);
        return;
    }

    APP_SendText(uart, "Unknown command. Type help.\r\n");
}

void APP_Console_Run(
    UART_HandleTypeDef *uart,
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT])
{
    uint8_t device;
    RAID5_Status result;
    char line[APP_LINE_SIZE];

    if ((uart == NULL) || (flashes == NULL))
    {
        return;
    }

    APP_SendText(
        uart,
        "\r\nSTM32F401RE RAID-5 Storage Console\r\n"
        "Checking W25Q64FV members...\r\n");

    for (device = 0U;
         device < RAID5_DEVICE_COUNT;
         device++)
    {
        uint8_t id[3] = {0U, 0U, 0U};

        HAL_StatusTypeDef flash_result =
            W25Q64_ReadID(&flashes[device], id);

        if ((flash_result == HAL_OK) &&
            (id[0] == 0xEFU) &&
            (id[1] == 0x40U) &&
            (id[2] == 0x17U))
        {
            (void)snprintf(
                line,
                sizeof(line),
                "Flash %u ID: %02X %02X %02X\r\n",
                (unsigned int)(device + 1U),
                (unsigned int)id[0],
                (unsigned int)id[1],
                (unsigned int)id[2]);
        }
        else
        {
            (void)snprintf(
                line,
                sizeof(line),
                "Flash %u unavailable/invalid ID: "
                "%02X %02X %02X\r\n",
                (unsigned int)(device + 1U),
                (unsigned int)id[0],
                (unsigned int)id[1],
                (unsigned int)id[2]);
        }

        APP_SendText(uart, line);
    }

    result = RAID5_Init(flashes);

    APP_PrintResult(uart, "RAID initialization", result);

    if (result != RAID5_OK)
    {
        APP_SendText(
            uart,
            "Cannot start storage: fewer than three valid members.\r\n");
        return;
    }

    APP_SendText(
        uart,
        "Startup does not erase or rewrite stored data.\r\n");

    APP_PrintStatus(uart);
    APP_PrintHelp(uart);

    while (1)
    {
        APP_SendText(uart, "\r\nRAID> ");

        if (APP_ReadLine(
                uart,
                line,
                sizeof(line)) != 0U)
        {
            APP_ProcessCommand(uart, line);
        }
    }
}
