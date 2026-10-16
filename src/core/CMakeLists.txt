configure_file(version.h.in ${PROJECT_BINARY_DIR}/generated/core/version.h @ONLY)

add_library(portside_core STATIC
    errors.h errors.cpp
    enums.h
    settings.h settings.cpp
    ports.h ports.cpp
    identity.h identity.cpp
    chips.h chips.cpp
    platform.h platform.cpp
    platform_linux.cpp
    platform_macos.cpp
    platform_windows.cpp
    platform_posix.cpp
    discovery.h discovery.cpp
    transport.h transport.cpp
    loopback_transport.h loopback_transport.cpp
    serial_transport.h serial_transport.cpp
    serial_errors.h serial_errors.cpp
    factory.h factory.cpp
    textio.h textio.cpp
    breaks.h breaks.cpp
    session.h session.cpp
    session_worker.h session_worker.cpp
    pacing.h pacing.cpp
    decoder.h decoder.cpp
    charwidth.h charwidth.cpp
    terminal.h terminal.cpp
    find.h find.cpp
    hexview.h hexview.cpp
    timestamps.h timestamps.cpp
    session_log.h session_log.cpp
    toml.h toml.cpp
    profiles.h profiles.cpp
)

# Everything includes "core/xxx.h", "gui/xxx.h" with src/ as the root.
target_include_directories(portside_core PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/generated)

target_link_libraries(portside_core
    PUBLIC Qt6::Core Qt6::SerialPort
    PRIVATE portside_warnings)
