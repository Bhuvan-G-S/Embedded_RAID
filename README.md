# STM32 RAID-5 Flash Storage

**An embedded storage system with data redundancy and single-device failure recovery, built using STM32 and external SPI flash memory.**

Built using an STM32F401RE microcontroller and four W25Q64FV SPI NOR flash devices, this project combines multiple memory chips into one logical storage system. It uses RAID-5-style data striping and rotating XOR parity to recover requested data when one flash device becomes unavailable.

The firmware includes a flash memory driver, a RAID-5 storage layer and a UART command-line interface for reading, writing and verifying stored data.

## Project Highlights

- **4 × 8 MB flash devices** managed through a shared SPI bus.
- **~23.95 MiB usable logical storage** after parity and reserved-sector overhead.
- **Single-member failure recovery** using XOR parity.
- **Rotating parity** distributed across all four flash devices.
- **Read-only degraded operation** when one member is unavailable.
- **UART storage console** for interacting with the system.
- **Parity verification** to check consistency between data and stored parity.

## 1. System Architecture

The STM32 acts as the storage controller. A PC communicates with it through a serial terminal using USART2. The firmware processes commands through the application layer, passes storage requests to the RAID engine, and communicates with the four flash chips through the SPI driver.

![System Architecture](docs/images/system-architecture.svg)

### Hardware connections

| Component | STM32 connection |
|---|---|
| SPI1 SCK | PA5 |
| SPI1 MISO | PA6 |
| SPI1 MOSI | PA7 |
| Flash 1 CS | PB6 |
| Flash 2 CS | PC7 |
| Flash 3 CS | PA9 |
| Flash 4 CS | PA8 |
| Serial interface | USART2, 115200 baud |

All four flash devices share SPI1. Each device has an independent chip-select signal, allowing the STM32 to communicate with them individually.

## 2. RAID-5 Data Organization

Each W25Q64FV device provides 8 MB of storage. The firmware divides the available memory into 4 KB blocks.

A group of four physical blocks forms one RAID stripe:

- Three blocks store application data.
- One block stores parity information calculated from the three data blocks.

Parity is calculated using the XOR operation:

`P = D0 XOR D1 XOR D2`

The parity location rotates between the four flash devices across successive stripes. This distributes parity storage instead of dedicating one device exclusively to parity.

![RAID-5 Data Layout](docs/images/raid5-data-layout.svg)

### Storage specifications

| Property | Value |
|---|---:|
| Microcontroller | STM32F401RE |
| Flash devices | 4 |
| Capacity per device | 8 MB |
| Total physical capacity | 32 MB |
| Flash sector size | 4 KB |
| Flash page size | 256 bytes |
| Reserved sectors per device | 4 |
| Available stripes | 2,044 |
| Data blocks per stripe | 3 |
| Parity blocks per stripe | 1 |
| Logical block size | 4 KB |
| Total logical blocks | 6,132 |
| Usable logical capacity | ~23.95 MiB |

The first four sectors of each flash device are reserved. RAID storage begins at address `0x4000`.

Logical block mapping:

| Stripe | Logical blocks |
|---|---|
| Stripe 0 | 0, 1, 2 |
| Stripe 1 | 3, 4, 5 |
| Stripe 2 | 6, 7, 8 |
| Stripe 3 | 9, 10, 11 |

## 3. Single-Member Failure Recovery

One of the main features of the system is its ability to reconstruct requested data when one physical flash device becomes unavailable.

For example, when Flash 1 is unavailable and contains data block D0, the firmware can reconstruct that block using the stored parity and the remaining two data blocks.

`D0 = P XOR D1 XOR D2`

The RAID layer reads the required surviving blocks, performs XOR operations and reconstructs the missing data in a RAM buffer.

![Single Member Failure Recovery](docs/images/read-recovery-flow.svg)

### Operating modes

| Mode | Behaviour |
|---|---|
| Normal | All four members available. Reads and writes supported. |
| Degraded | One member unavailable. Reads and XOR recovery supported; writes disabled. |
| Initialization failure | Fewer than three valid members detected. RAID initialization fails. |

The system requires at least three available members to perform RAID reads.

**Recovery is performed in RAM.** The current implementation does not automatically rebuild data onto a replacement flash device.

## 4. Firmware Modules

The firmware is organized into separate modules to keep hardware communication, storage management and user interaction independent.

| Module | Responsibility |
|---|---|
| `main.c` / `main.h` | Hardware initialization and application entry point |
| `w25q64.c` / `w25q64.h` | SPI flash driver: identification, read, program, erase and status polling |
| `raid5.c` / `raid5.h` | Data mapping, parity calculation, logical reads/writes and recovery |
| `app_demo.c` / `app_demo.h` | UART command-line interface |

### Flash driver

The W25Q64 driver provides the basic operations required to access external NOR flash memory:

- Read JEDEC identification.
- Read the flash status register.
- Wait for internal programming and erase operations to finish.
- Read memory.
- Program data in page-safe chunks.
- Erase 4 KB sectors.

### RAID storage layer

The RAID layer provides a logical storage interface independent of the physical flash locations.

It handles:

- Logical-to-physical block mapping.
- Rotating parity placement.
- XOR parity generation.
- Stripe-level writes.
- Normal and degraded reads.
- Single-member data reconstruction.
- Stripe parity verification.

### UART console

The application layer provides a simple serial interface to interact with the storage system. It accepts commands from a serial terminal and displays operation results.

## 5. Using the Serial Console

Connect the STM32 to a PC and open a serial terminal using:

| Setting | Value |
|---|---|
| Interface | USART2 |
| Baud rate | 115200 |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Flow control | None |

### Available commands

| Command | Description |
|---|---|
| `help` | Display available commands |
| `status` | Show flash member availability and RAID mode |
| `write <block> <text>` | Store text in a logical block |
| `read <block>` | Retrieve text from a logical block |
| `verify <block>` | Verify parity for the block's stripe |

### Example

```text
help
status

write 0 Hello from STM32 RAID5
read 0
verify 0

write 1 Embedded storage project
read 1
verify 1
```

The console accepts up to 120 characters per text write.

Text records use a small header inside the 4 KB logical block:

| Byte range | Contents |
|---|---|
| 0–3 | `TXT1` record identifier |
| 4–5 | Text length (16-bit little-endian) |
| 6 onwards | Text payload |
| Remaining bytes | `0xFF` |

The text limit belongs to the console interface. The underlying RAID storage functions operate on complete 4 KB blocks.

Normal startup does not automatically erase stored data.

## 6. Testing and Validation

The project has been tested with the following operations:

- JEDEC identification of the four W25Q64FV devices (`EF 40 17`).
- RAID initialization and member availability detection.
- Stripe writing and logical block readback.
- XOR parity generation and verification.
- Degraded-mode logical reads.
- Physical disconnection testing of a flash member.

In a physical failure test, Flash 4 was disconnected and all nine logical blocks across stripes 0, 1 and 2 remained readable. This included recovery of data stored on Flash 4 in stripes where it held data rather than parity.

These tests demonstrate the core storage and recovery behaviour, but do not represent exhaustive endurance, performance or reliability qualification.

## 7. Current Limitations

This is an embedded storage prototype intended to demonstrate RAID concepts and firmware development.

The current implementation does not include:

- Automatic rebuilding onto a replacement flash device.
- Hot-swapping support.
- A filesystem or USB mass-storage interface.
- Power-loss protection or transactional stripe updates.
- Persistent member identity and automatic detection of swapped devices.
- Checksums for detecting arbitrary corruption in every data block.

A logical block write reads the other two blocks in its stripe, calculates new parity, erases the corresponding sector on all four devices and programs the complete stripe. A failure or power interruption during this sequence may leave an incomplete stripe.

Member availability is checked using the expected JEDEC ID. This confirms that a device responds with the expected identification; it does not verify the health of every memory location.

## 8. Technologies Used

**Hardware:** STM32F401RE, W25Q64FV SPI NOR Flash

**Firmware:** Embedded C, STM32 HAL

**Communication:** SPI, UART

**Storage algorithm:** RAID-5-style striping, XOR parity and single-member recovery

**Development tools:** STM32CubeIDE, STM32CubeMX, ST-Link, serial terminal, Git

## Project Goal

The goal of this project was to build an embedded storage system from the hardware interface upwards: starting with SPI flash communication, developing a reusable flash driver, implementing RAID-5 storage management, and validating single-device failure recovery on a microcontroller.

It demonstrates practical embedded C development, peripheral communication, external memory management, storage algorithms and hardware-level fault handling.
