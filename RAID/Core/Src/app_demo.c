#include "app_demo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Maximum number of characters accepted for one UART command.
 * Includes space for the terminating '\0'.
 */
#define APP_LINE_SIZE       160U

/* Maximum text stored in one logical block through the console. */
#define APP_MAX_TEXT_LENGTH 120U

/*
 * Text record layout:
 * Bytes 0-3: "TXT1" identifier
 * Bytes 4-5: Text length (little-endian)
 * Byte 6 onwards: Actual text
 */
#define APP_RECORD_HEADER   6U


/*
 * Temporary 4 KB buffer used when reading or writing a logical block.
 *
 * Static storage keeps this buffer outside the function call stack.
 * The same buffer is reused by the console commands.
 */
static uint8_t s_cli_block[RAID5_BLOCK_SIZE];

/*
 * Tracks whether the previous UART line ended with '\r'.
 *
 * Some serial terminals send "\r\n" when Enter is pressed.
 * After receiving '\r', the next '\n' is ignored so that
 * one Enter key press produces only one command.
 */
static uint8_t s_ignore_lf = 0U;


/*
 * Identifier used to recognize text records written through
 * this UART console.
 *
 * It helps distinguish console text records from raw or
 * previously stored binary data.
 */
static const uint8_t s_text_magic[4] = {
    'T', 'X', 'T', '1'
};


/*
 * Send a null-terminated string to the PC through UART.
 *
 * Parameters:
 * uart -> UART peripheral used for transmission.
 * text -> String to transmit.
 *
 * strlen() calculates the number of characters to send,
 * excluding the terminating '\0'.
 */
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


/*
 * Print the result of a RAID operation through UART.
 *
 * RAID5_StatusString() converts the RAID status code into
 * a readable string such as "OK" or "member unavailable".
 *
 * snprintf() formats the operation name and result into
 * a temporary string before sending it to the PC.
 */
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


/*
 * Receive one complete command from the PC.
 *
 * Characters are received one at a time and stored in 'line'.
 *
 * This function also:
 * - Echoes received characters back to the terminal.
 * - Handles Enter key line endings.
 * - Supports backspace.
 * - Accepts printable ASCII characters.
 * - Prevents writing beyond the supplied buffer capacity.
 *
 * Returns:
 * 1 -> A complete line was received.
 * 0 -> Invalid function arguments.
 */
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

        /*
         * Receive one byte from USART2.
         * HAL_MAX_DELAY makes this a blocking receive operation.
         */
        if (HAL_UART_Receive(
                uart,
                &ch,
                1U,
                HAL_MAX_DELAY) != HAL_OK)
        {
            continue;
        }


        /*
         * If the previous character was '\r', ignore a following
         * '\n'. This handles terminals that send CR+LF.
         */
        if (s_ignore_lf != 0U)
        {
            s_ignore_lf = 0U;

            if (ch == '\n')
            {
                continue;
            }
        }


        /* Carriage return marks the end of the command. */
        if (ch == '\r')
        {
            s_ignore_lf = 1U;

            /* Add the null terminator to complete the C string. */
            line[length] = '\0';

            APP_SendText(uart, "\r\n");

            return 1U;
        }


        /* Also accept terminals that send only line feed. */
        if (ch == '\n')
        {
            line[length] = '\0';

            APP_SendText(uart, "\r\n");

            return 1U;
        }


        /*
         * Handle Backspace and Delete.
         *
         * Reduce the stored character count by one and update
         * the terminal display.
         */
        if ((ch == '\b') || (ch == 0x7FU))
        {
            if (length > 0U)
            {
                length--;

                APP_SendText(uart, "\b \b");
            }

            continue;
        }


        /*
         * Accept printable ASCII characters only.
         *
         * capacity - 1 leaves one byte for the terminating '\0'.
         */
        if ((ch >= 32U) && (ch <= 126U) &&
            (length < (capacity - 1U)))
        {
            line[length++] = (char)ch;

            /* Echo the received character to the terminal. */
            (void)HAL_UART_Transmit(
                uart,
                &ch,
                1U,
                HAL_MAX_DELAY);
        }
    }
}


/*
 * Convert the block number entered in a command into uint32_t.
 *
 * Example:
 * Input: "5"
 * Output: block = 5
 *
 * end_position points to the character immediately after
 * the parsed number. The command parser uses it to check
 * whether additional text follows the block number.
 *
 * Returns:
 * 1 -> Valid block number.
 * 0 -> Invalid number or block outside the RAID address range.
 */
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


    /*
     * Convert the decimal number in the command into
     * an unsigned long integer.
     *
     * Base 10 means the number is interpreted as decimal.
     * 'end' receives the address of the first character
     * that was not part of the number.
     */
    value = strtoul(text, &end, 10);

    if (end == text)
    {
        return 0U;
    }


    /* Reject logical block numbers outside the RAID capacity. */
    if (value >= RAID5_LOGICAL_BLOCK_COUNT)
    {
        return 0U;
    }


    /* Store the converted number in the caller's variable. */
    *block = (uint32_t)value;


    /*
     * Return where number parsing stopped.
     *
     * For "write 5 Hello", this points to the space
     * immediately after the number 5.
     */
    if (end_position != NULL)
    {
        *end_position = end;
    }

    return 1U;
}


/*
 * Display the current RAID storage status.
 *
 * First, refresh the availability of all four physical flash
 * devices. This avoids relying only on the member status saved
 * during RAID initialization.
 *
 * Then display each member's availability and the current
 * storage operating mode.
 */
static void APP_PrintStatus(UART_HandleTypeDef *uart)
{
    uint8_t device;
    uint8_t online = 0U;

    char line[128];

    /*
     * Recheck all four flash chips by reading their JEDEC IDs.
     * The RAID layer updates its member availability array.
     */
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


    /* Display the status of each physical flash member. */
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


    /*
     * Display the total number of available members,
     * logical storage capacity and current operating mode.
     */
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


/*
 * Read a logical block and display its stored text.
 *
 * The RAID layer handles the physical flash mapping.
 * If the required data member is unavailable, the RAID layer
 * attempts to reconstruct the block using XOR parity.
 *
 * After reading, this function checks whether the block
 * contains a text record created by this console.
 */
static void APP_ReadCommand(
    UART_HandleTypeDef *uart,
    uint32_t logical_block)
{
    RAID5_Status result;
    uint16_t text_length;
    char line[100];


    /* Read the requested logical block into the RAM buffer. */
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


    /*
     * Check the first four bytes for the "TXT1" identifier.
     *
     * If the identifier does not match, the block may contain
     * raw data or data that was not written through this console.
     */
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


    /*
     * Extract the text length from bytes 4 and 5.
     *
     * The length is stored in little-endian format:
     * Byte 4 contains the lower 8 bits.
     * Byte 5 contains the upper 8 bits.
     */
    text_length =
        (uint16_t)s_cli_block[4] |
        ((uint16_t)s_cli_block[5] << 8);


    /*
     * Make sure the stored length does not extend beyond
     * the available text area of the 4 KB block.
     */
    if (text_length >
        (RAID5_BLOCK_SIZE - APP_RECORD_HEADER))
    {
        APP_SendText(uart, "Invalid text record length.\r\n");
        return;
    }


    /* Display the text stored after the six-byte record header. */
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


/*
 * Store a text message in a logical block.
 *
 * The message is packaged into a 4 KB buffer:
 *
 * Bytes 0-3 : "TXT1" identifier
 * Bytes 4-5 : Text length
 * Remaining: Text followed by unused 0xFF bytes
 *
 * The complete buffer is then passed to the RAID layer.
 * The RAID layer handles block mapping and parity calculation.
 */
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


    /* Calculate the number of characters in the message. */
    text_length = strlen(text);

    if (text_length == 0U)
    {
        APP_SendText(uart, "Text cannot be empty.\r\n");
        return;
    }


    /* Enforce the console's 120-character text limit. */
    if (text_length > APP_MAX_TEXT_LENGTH)
    {
        APP_SendText(
            uart,
            "Text is too long. Maximum is 120 characters.\r\n");

        return;
    }


    /*
     * Fill the complete block with 0xFF.
     *
     * Unused space remains erased-value data.
     * This also ensures that bytes from an earlier message
     * do not remain in the new record.
     */
    memset(s_cli_block, 0xFF, sizeof(s_cli_block));


    /* Store the text record identifier in the first four bytes. */
    memcpy(
        s_cli_block,
        s_text_magic,
        sizeof(s_text_magic));


    /*
     * Store the text length as a 16-bit little-endian value.
     * Splitting it into two bytes allows the read function
     * to recover the original message length.
     */
    s_cli_block[4] = (uint8_t)(text_length & 0xFFU);
    s_cli_block[5] = (uint8_t)((text_length >> 8) & 0xFFU);


    /* Copy the actual message immediately after the header. */
    memcpy(
        &s_cli_block[APP_RECORD_HEADER],
        text,
        text_length);


    /*
     * Pass the complete logical block to the RAID layer.
     *
     * RAID5_WriteBlock() preserves the other two data blocks
     * in the stripe and recalculates parity.
     */
    result = RAID5_WriteBlock(
        logical_block,
        s_cli_block);

    APP_PrintResult(uart, "Write", result);
}


/*
 * Verify the parity of the stripe containing a logical block.
 *
 * Three logical blocks belong to each stripe, so dividing
 * the logical block number by RAID5_DATA_BLOCKS gives
 * the corresponding stripe index.
 *
 * RAID5_VerifyStripe() recalculates XOR parity from the
 * stored data and compares it with the stored parity block.
 */
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


/*
 * Display the commands supported by the UART console.
 *
 * The final logical block number is calculated from the
 * RAID configuration instead of being hardcoded.
 */
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


    /* Display the highest valid logical block number. */
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


/*
 * Identify and execute the command received from the PC.
 *
 * Supported commands:
 * help
 * status
 * read <block>
 * write <block> <text>
 * verify <block>
 *
 * Each command is checked and its arguments are extracted
 * before calling the corresponding application function.
 */
static void APP_ProcessCommand(
    UART_HandleTypeDef *uart,
    char *line)
{
    char *command = line;


    /*
     * Skip spaces at the beginning of the command.
     * This allows commands such as "  status".
     */
    while (*command == ' ')
    {
        command++;
    }


    /*
     * Remove spaces at the end of the command.
     *
     * The block is enclosed in its own scope so that the
     * temporary length variable does not remain in use
     * outside this trimming operation.
     */
    {
        size_t length = strlen(command);

        while ((length > 0U) &&
               (command[length - 1U] == ' '))
        {
            command[length - 1U] = '\0';
            length--;
        }
    }


    /* Ignore an empty command. */
    if (command[0] == '\0')
    {
        return;
    }


    /* Execute the help command. */
    if (strcmp(command, "help") == 0)
    {
        APP_PrintHelp(uart);
        return;
    }


    /* Refresh and display the physical flash member status. */
    if (strcmp(command, "status") == 0)
    {
        APP_PrintStatus(uart);
        return;
    }


    /*
     * READ COMMAND
     *
     * Expected input:
     * read 5
     *
     * command + 5 skips the characters "read ".
     * APP_ParseBlockNumber() extracts the logical block number.
     */
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


    /*
     * WRITE COMMAND
     *
     * Expected input:
     * write 5 Hello STM32
     *
     * First, extract the block number.
     * Then locate the text that follows it.
     */
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


        /* A space must separate the block number and message. */
        if (*end_position != ' ')
        {
            APP_SendText(
                uart,
                "Usage: write <logical_block> <text>\r\n");

            return;
        }


        /* Locate the beginning of the text. */
        text = end_position;

        /* Skip spaces between the block number and text. */
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


    /*
     * VERIFY COMMAND
     *
     * Expected input:
     * verify 5
     *
     * The logical block is converted into its stripe index
     * by APP_VerifyCommand().
     */
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


    /* Display an error if no supported command matches. */
    APP_SendText(uart, "Unknown command. Type help.\r\n");
}


/*
 * Start the interactive RAID-5 UART console.
 *
 * This function:
 * 1. Checks the supplied UART and flash configuration.
 * 2. Reads the JEDEC ID from each flash device.
 * 3. Initializes the RAID layer.
 * 4. Displays storage status and available commands.
 * 5. Waits for commands and sends them to the command parser.
 *
 * The function contains an infinite command loop and normally
 * does not return to main().
 */
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


    /*
     * Read the JEDEC ID of each physical flash chip.
     *
     * A valid W25Q64FV is expected to return:
     * EF 40 17
     *
     * This loop displays the identification result.
     * RAID5_Init() performs its own member detection and
     * stores the availability state used by the RAID layer.
     */
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


    /*
     * Initialize the RAID layer.
     *
     * The RAID layer checks member availability and prepares
     * the internal mapping used for logical block operations.
     *
     * It accepts normal operation with four available members
     * or degraded read operation with one unavailable member.
     */
    result = RAID5_Init(flashes);

    APP_PrintResult(uart, "RAID initialization", result);


    /*
     * Stop the console if fewer than three valid members
     * are available. RAID-5 cannot reconstruct data with
     * two or more missing members.
     */
    if (result != RAID5_OK)
    {
        APP_SendText(
            uart,
            "Cannot start storage: fewer than three valid members.\r\n");

        return;
    }


    /*
     * Starting the console does not format the flash memory.
     * Existing stored data remains untouched.
     */
    APP_SendText(
        uart,
        "Startup does not erase or rewrite stored data.\r\n");


    /* Display the initial member status and command list. */
    APP_PrintStatus(uart);
    APP_PrintHelp(uart);


    /*
     * Main console loop.
     *
     * Wait for one command, read it into 'line',
     * and pass it to APP_ProcessCommand().
     *
     * This repeats for as long as the storage console runs.
     */
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
