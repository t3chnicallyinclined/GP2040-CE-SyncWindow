// Minimal W6100 Ethernet driver for UDP gamepad input.
// Only implements: SPI init, chip reset, static IP config, UDP socket 0 receive.
// Hardware: W6100-EVB-Pico2 (GPIO 16=MISO, 17=CSn, 18=SCLK, 19=MOSI, 20=RSTn, 21=INTn)

#pragma once
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// W6100 Register Addresses (common)
// ============================================================
#define W6100_MR            0x4000   // Mode Register
#define W6100_SHAR          0x4120   // Source MAC Address (6 bytes)
#define W6100_GAR           0x4130   // Gateway IP (4 bytes)
#define W6100_SUBR          0x4134   // Subnet Mask (4 bytes)
#define W6100_SIPR          0x4138   // Source IP (4 bytes)
#define W6100_PHYCFGR       0x3000   // PHY Config Register
#define W6100_CHPLCKR       0x41F4   // Chip Lock Register
#define W6100_NETLCKR       0x41F5   // Network Lock Register
#define W6100_PHYLCKR       0x41F6   // PHY Lock Register
#define W6100_SYSR          0x2000   // System Status Register
#define W6100_VERSIONR      0x0000   // Version Register (also try 0x0002 per io6Library)

// ============================================================
// W6100 Socket Register Addresses (socket 0)
// ============================================================
#define W6100_Sn_MR(n)      (0x0000 + (n)*0x0100)  // Socket Mode
#define W6100_Sn_CR(n)      (0x0010 + (n)*0x0100)  // Socket Command
#define W6100_Sn_SR(n)      (0x0030 + (n)*0x0100)  // Socket Status
#define W6100_Sn_PORTR(n)   (0x0114 + (n)*0x0100)  // Socket Port (2 bytes)
#define W6100_Sn_RX_BSR(n)  (0x0220 + (n)*0x0100)  // RX Buffer Size
#define W6100_Sn_RX_RSR(n)  (0x0224 + (n)*0x0100)  // RX Received Size (2 bytes)
#define W6100_Sn_RX_RD(n)   (0x0228 + (n)*0x0100)  // RX Read Pointer (2 bytes)
#define W6100_Sn_TX_BSR(n)  (0x0200 + (n)*0x0100)  // TX Buffer Size
#define W6100_Sn_TX_FSR(n)  (0x0204 + (n)*0x0100)  // TX Free Size

// Socket block select bits
#define W6100_BSB_SOCK_REG(n)   ((n)*4 + 1)    // Socket n register block
#define W6100_BSB_SOCK_TX(n)    ((n)*4 + 2)     // Socket n TX buffer
#define W6100_BSB_SOCK_RX(n)    ((n)*4 + 3)     // Socket n RX buffer
#define W6100_BSB_COMMON        0               // Common register block

// Socket commands
#define W6100_Sn_CR_OPEN    0x01
#define W6100_Sn_CR_CLOSE   0x10
#define W6100_Sn_CR_RECV    0x40

// Socket modes
#define W6100_Sn_MR_UDP4    0x02   // UDP IPv4

// Socket status
#define W6100_SOCK_UDP      0x22

// SPI frame control bits
#define W6100_SPI_READ      0x00
#define W6100_SPI_WRITE     0x04

// ============================================================
// W6100 Driver API
// ============================================================

#ifdef __cplusplus
extern "C" {
#endif

// Initialize W6100 SPI and chip. Returns true if chip detected.
bool w6100_init(unsigned int pin_miso, unsigned int pin_cs, unsigned int pin_sclk, unsigned int pin_mosi, unsigned int pin_rst);

// Configure static IP. All params in network byte order (big-endian).
void w6100_set_ip(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4]);

// Set MAC address.
void w6100_set_mac(const uint8_t mac[6]);

// Open UDP socket 0 on specified port. Returns true if socket opened.
bool w6100_udp_open(uint16_t port);

// Poll for received UDP data on socket 0.
// Returns number of data bytes available (excluding 8-byte packet header).
// If > 0, fills src_ip (4 bytes), src_port, and data (up to max_len).
int w6100_udp_recv(uint8_t* data, uint16_t max_len, uint8_t* src_ip, uint16_t* src_port);

// Check if Ethernet link is up.
bool w6100_link_up(void);

// Read chip version register (should return 0x46 for W6100).
uint8_t w6100_get_version(void);

#ifdef __cplusplus
}
#endif
