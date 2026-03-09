set(ESP_ETH_IDF_COMPONENT_DIR "$ENV{IDF_PATH}/components/esp_eth")

idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    return() # This component is not supported by the POSIX/Linux simulator
endif()

idf_build_get_property(components_to_build BUILD_COMPONENTS)

set(srcs)
set(include "${ESP_ETH_IDF_COMPONENT_DIR}/include")
set(ld_fragments "${ESP_ETH_IDF_COMPONENT_DIR}/linker.lf")
# Mirror the upstream esp_eth component and keep the project-local override limited
# to the QEMU OpenETH ISR implementation in src/openeth/esp_eth_mac_openeth.c.
set(priv_requires log esp_timer esp_driver_spi esp_driver_gpio)

if(CONFIG_ETH_ENABLED)
    set(srcs
        "${ESP_ETH_IDF_COMPONENT_DIR}/src/esp_eth.c"
        "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_802_3.c"
    )

    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        if(esp_netif IN_LIST components_to_build)
            list(APPEND srcs "${ESP_ETH_IDF_COMPONENT_DIR}/src/esp_eth_netif_glue.c")
        endif()
    endif()

    if(CONFIG_ETH_USE_ESP32_EMAC)
        list(APPEND srcs
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/mac/esp_eth_mac_esp.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/mac/esp_eth_mac_esp_dma.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/mac/esp_eth_mac_esp_gpio.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_generic.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_dp83848.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_ip101.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_ksz80xx.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_lan87xx.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_rtl8201.c"
        )
    endif()

    if(CONFIG_ETH_SPI_ETHERNET_DM9051)
        list(APPEND srcs
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/dm9051/esp_eth_mac_dm9051.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/dm9051/esp_eth_phy_dm9051.c"
        )
    endif()

    if(CONFIG_ETH_SPI_ETHERNET_W5500)
        list(APPEND srcs
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/w5500/esp_eth_mac_w5500.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/w5500/esp_eth_phy_w5500.c"
        )
    endif()

    if(CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL)
        list(APPEND srcs
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/ksz8851snl/esp_eth_mac_ksz8851snl.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/spi/ksz8851snl/esp_eth_phy_ksz8851snl.c"
        )
    endif()

    if(CONFIG_ETH_USE_OPENETH)
        list(APPEND srcs
            "src/openeth/esp_eth_mac_openeth.c"
            "${ESP_ETH_IDF_COMPONENT_DIR}/src/phy/esp_eth_phy_dp83848.c"
        )
    endif()
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "${include}"
    PRIV_INCLUDE_DIRS "${ESP_ETH_IDF_COMPONENT_DIR}/src/openeth"
    LDFRAGMENTS "${ld_fragments}"
    REQUIRES esp_event
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_ETH_ENABLED)
    if(CONFIG_ETH_USE_SPI_ETHERNET)
        idf_component_optional_requires(PUBLIC esp_driver_spi)
    endif()
    idf_component_optional_requires(PRIVATE esp_netif esp_pm esp_mm)
endif()
