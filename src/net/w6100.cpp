// W6100 Ethernet driver using WIZnet ioLibrary_Driver + DHCP.

#include "net/w6100.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "w6100.h"
#include "dhcp.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>

static spi_inst_t* w6100_spi = spi0;
static uint w6100_cs_pin = 17;

// DHCP buffer (must be 548+ bytes per DHCP spec)
static uint8_t dhcp_buf[1024];
#define DHCP_SOCKET 7  // Use socket 7 for DHCP (socket 0 for UDP gamepad data)

// Stored IP after DHCP
static uint8_t assigned_ip[4] = {0};
static bool dhcp_done = false;

// ============================================================
// SPI callbacks for ioLibrary
// ============================================================

static void spi_cs_select(void) { gpio_put(w6100_cs_pin, 0); }
static void spi_cs_deselect(void) { gpio_put(w6100_cs_pin, 1); }
static uint8_t spi_read_byte(void) {
    uint8_t rx;
    spi_read_blocking(w6100_spi, 0x00, &rx, 1);
    return rx;
}
static void spi_write_byte(uint8_t val) {
    spi_write_blocking(w6100_spi, &val, 1);
}
static void spi_read_burst(uint8_t* buf, datasize_t len) {
    spi_read_blocking(w6100_spi, 0x00, buf, len);
}
static void spi_write_burst(uint8_t* buf, datasize_t len) {
    spi_write_blocking(w6100_spi, buf, len);
}

// DHCP callbacks
static void dhcp_ip_assign(void) {
    getIPfromDHCP(assigned_ip);
    uint8_t gw[4], sn[4], dns[4];
    getGWfromDHCP(gw);
    getSNfromDHCP(sn);
    getDNSfromDHCP(dns);
    setSIPR(assigned_ip);
    setGAR(gw);
    setSUBR(sn);
    dhcp_done = true;
}

static void dhcp_ip_update(void) {
    dhcp_ip_assign();  // Same handler
}

static void dhcp_ip_conflict(void) {
    // IP conflict — nothing we can do, just retry
}

// 1-second timer for DHCP
static volatile uint32_t dhcp_tick = 0;
static uint32_t last_tick_ms = 0;

// ============================================================
// Public API
// ============================================================

bool w6100_init(unsigned int pin_miso, unsigned int pin_cs, unsigned int pin_sclk, unsigned int pin_mosi, unsigned int pin_rst) {
    w6100_cs_pin = pin_cs;

    // Init SPI0 at 33MHz
    spi_init(w6100_spi, 33 * 1000 * 1000);
    spi_set_format(w6100_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(pin_sclk, GPIO_FUNC_SPI);
    gpio_set_function(pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(pin_miso, GPIO_FUNC_SPI);

    gpio_init(pin_cs);
    gpio_set_dir(pin_cs, GPIO_OUT);
    gpio_put(pin_cs, 1);

    gpio_init(pin_rst);
    gpio_set_dir(pin_rst, GPIO_OUT);
    gpio_put(pin_rst, 1);

    // Hardware reset
    gpio_put(pin_rst, 0);
    sleep_us(10);
    gpio_put(pin_rst, 1);
    sleep_ms(100);  // Datasheet says 60.3ms

    // Register SPI callbacks
    reg_wizchip_cs_cbfunc(spi_cs_select, spi_cs_deselect);
    reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte, spi_read_burst, spi_write_burst);

    // Set socket buffer sizes
    uint8_t txsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t rxsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    wizchip_init(txsize, rxsize);

    // Unlock registers
    uint8_t lock = SYS_CHIP_LOCK | SYS_NET_LOCK | SYS_PHY_LOCK;
    ctlwizchip(CW_SYS_UNLOCK, &lock);

    // Set MAC address
    uint8_t mac[6] = {0x00, 0x08, 0xDC, 0xDC, 0x00, 0x01};
    setSHAR(mac);

    // Run DHCP to get IP automatically
    reg_dhcp_cbfunc(dhcp_ip_assign, dhcp_ip_update, dhcp_ip_conflict);
    DHCP_init(DHCP_SOCKET, dhcp_buf);

    // Run DHCP loop — give it up to 10 seconds
    dhcp_done = false;
    last_tick_ms = to_ms_since_boot(get_absolute_time());

    for (int attempt = 0; attempt < 200 && !dhcp_done; attempt++) {
        // Tick the 1-second timer
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_tick_ms >= 1000) {
            DHCP_time_handler();
            last_tick_ms = now_ms;
        }

        uint8_t ret = DHCP_run();
        if (ret == DHCP_IP_LEASED || ret == DHCP_IP_ASSIGN) {
            dhcp_done = true;
            break;
        }
        if (ret == DHCP_FAILED) {
            break;
        }
        sleep_ms(50);
    }

    if (!dhcp_done) {
        // DHCP failed — fall back to static IP
        uint8_t ip[4] = {192, 168, 1, 100};
        uint8_t subnet[4] = {255, 255, 255, 0};
        uint8_t gateway[4] = {192, 168, 1, 1};
        setSIPR(ip);
        setSUBR(subnet);
        setGAR(gateway);
    }

    // Lock network registers
    lock = SYS_NET_LOCK;
    ctlwizchip(CW_SYS_LOCK, &lock);

    return true;
}

void w6100_set_mac(const uint8_t mac[6]) {
    setSHAR((uint8_t*)mac);
}

void w6100_set_ip(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4]) {
    uint8_t lock = SYS_NET_LOCK;
    ctlwizchip(CW_SYS_UNLOCK, &lock);
    setGAR((uint8_t*)gateway);
    setSUBR((uint8_t*)subnet);
    setSIPR((uint8_t*)ip);
    ctlwizchip(CW_SYS_LOCK, &lock);
}

bool w6100_udp_open(uint16_t port) {
    close(0);
    sleep_ms(1);
    int8_t ret = socket(0, Sn_MR_UDP4, port, 0);
    if (ret != 0) return false;
    return (getSn_SR(0) == SOCK_UDP);
}

int w6100_udp_recv(uint8_t* data, uint16_t max_len, uint8_t* src_ip, uint16_t* src_port) {
    uint16_t rx_size = getSn_RX_RSR(0);
    if (rx_size == 0) return 0;

    uint8_t dest_ip[4];
    uint16_t dest_port;
    int32_t len = recvfrom(0, data, max_len, dest_ip, &dest_port);

    if (src_ip) memcpy(src_ip, dest_ip, 4);
    if (src_port) *src_port = dest_port;

    return (len > 0) ? len : 0;
}

bool w6100_link_up(void) {
    return (getPHYSR() & 0x01) != 0;
}

uint8_t w6100_get_version(void) {
    return (uint8_t)(getVER() >> 8);
}
