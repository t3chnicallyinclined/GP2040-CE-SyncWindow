add_library(ioLibrary_Driver STATIC
    Ethernet/wizchip_conf.c
    Ethernet/socket.c
    Ethernet/W6100/w6100.c
    Internet/DHCP/dhcp.c
)

target_include_directories(ioLibrary_Driver PUBLIC
    Ethernet
    Ethernet/W6100
    Internet/DHCP
)

# Chip select: W6100
target_compile_definitions(ioLibrary_Driver PUBLIC
    _WIZCHIP_=W6100
)

# Treat pointer type warnings as non-fatal
target_compile_options(ioLibrary_Driver PRIVATE -Wno-incompatible-pointer-types)
