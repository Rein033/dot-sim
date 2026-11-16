/**
 * @file kemu_net.c
 * @brief A network for the simulated Kode Dot: the P4's own Ethernet.
 *
 * On the board the network is the C5's WiFi, over ESP-Hosted, and esp-emu
 * cannot run that C5 (its ESP-Hosted bridge takes only a C6 as the slave).
 * It does model the P4's built-in EMAC, with an IP101 on the MDIO bus, on
 * its user-mode network: DHCP gives 192.168.4.2, and esp-emu's hostfwd is
 * how the Mac reaches a port. kemu brings that up when an app asks for WiFi
 * and kemu_radio.c reports it in the radio's own signals, so the app sees
 * what it sees on the board and its lwIP sockets carry the traffic.
 *
 * The EMAC's pins are the Kode Dot's other things; the ones chosen below are
 * those the simulator leaves unclaimed, never one a chip of kode-sim drives.
 */
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "kemu_net.h"

static const char *TAG = "kemu.net";

static kemu_net_cb_t s_on_ip;
static bool s_started;

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t const *const ev = data;
    if (s_on_ip != NULL) {
        s_on_ip(ev->ip_info.ip.addr);
    }
}

static bool ok_or_done(esp_err_t err)
{
    return (err == ESP_OK) || (err == ESP_ERR_INVALID_STATE);   /* someone did it already */
}

bool kemu_net_start(kemu_net_cb_t on_ip)
{
    if (s_started) {
        return true;
    }
    s_on_ip = on_ip;
    if (!ok_or_done(esp_netif_init()) || !ok_or_done(esp_event_loop_create_default())) {
        return false;
    }
    esp_netif_config_t const netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *const netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t const mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* The default SMI pins are the NFC reader's SPI (31) and the C5's SDIO
     * (52), and the SPI driver has them reserved: the MDIO bus goes through
     * the GPIO matrix to the dock's pads (26, 27), whose only owners are the
     * dock's USB console, which kemu_usb.c answers "not supported", and the
     * station's pads-loop factory test. esp-emu's PHY answers on any pins. */
    emac_cfg.smi_gpio.mdc_num = 26;
    emac_cfg.smi_gpio.mdio_num = 27;
    /* The RMII lines take only fixed IO_MUX pins (IDF's esp32p4
     * emac_periph.c), and every choice of each is one of the Kode Dot's:
     *  - CLK, TX_EN, CRS_DV, RXD0, RXD1: 50, 49, 51, 52, 53, the C5's SDIO,
     *    which nothing drives in a build without the radio and esp-hosted
     *    (sdkconfig.emu). The defaults' CRS_DV and RXD1 (28, 30) are the NFC
     *    reader's SPI, reserved;
     *  - TXD0, TXD1: 41, 42, the microSD's D2 and D3, which nothing claims
     *    because kemu_sd.c stands in for the SDMMC host. The other choices,
     *    34 and 35, are the I2S clock and the TOP button, which kode-sim's
     *    chips drive and kdrv_buttons reads: an EMAC there takes TOP away
     *    from every app that starts WiFi. */
    emac_cfg.clock_config.rmii.clock_gpio = 50;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num = 49;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num = 41;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num = 42;
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = 51;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num = 52;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num = 53;
    esp_eth_mac_t *const mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = -1;                 /* GPIO51 is the C5's SD_CMD on the board */
    esp_eth_phy_t *const phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_handle_t eth = NULL;
    esp_eth_config_t const eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if ((netif == NULL) || (mac == NULL) || (phy == NULL)
        || (esp_eth_driver_install(&eth_cfg, &eth) != ESP_OK)
        || (esp_netif_attach(netif, esp_eth_new_netif_glue(eth)) != ESP_OK)
        || (esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL) != ESP_OK)
        || (esp_eth_start(eth) != ESP_OK)) {
        ESP_LOGE(TAG, "the emulated Ethernet did not come up");
        return false;
    }
    s_started = true;
    ESP_LOGI(TAG, "the emulated Ethernet is up; waiting for DHCP");
    return true;
}
