# One Qt Test executable per area, each registered with CTest.  The GUI tests
# run headless: CTest sets QT_QPA_PLATFORM=offscreen for them.

add_library(portside_test_support STATIC
    support/support.h support/support.cpp
    support/socat_pair.h support/socat_pair.cpp
    support/fake_switch.h support/fake_switch.cpp)
target_include_directories(portside_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(portside_test_support PUBLIC portside_core Qt6::Test PRIVATE portside_warnings)

# portside_add_test(<name> <source> [LIBS ...] [LABELS ...] [GUI])
function(portside_add_test name source)
    cmake_parse_arguments(ARG "GUI" "" "LIBS;LABELS" ${ARGN})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE portside_test_support ${ARG_LIBS} portside_warnings)
    add_test(NAME ${name} COMMAND ${name})
    set(environment
        "PORTSIDE_CLI=$<TARGET_FILE:portside>"
        "QT_LOGGING_RULES=qt.qpa.*=false")
    if(ARG_GUI)
        list(APPEND environment "QT_QPA_PLATFORM=offscreen")
        list(APPEND ARG_LABELS gui)
    endif()
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "${environment}"
        LABELS "${ARG_LABELS}"
        TIMEOUT 300)
endfunction()

portside_add_test(tst_settings core/tst_settings.cpp LABELS core)
portside_add_test(tst_discovery core/tst_discovery.cpp LABELS core)
portside_add_test(tst_errors core/tst_errors.cpp LABELS core)
portside_add_test(tst_textio core/tst_textio.cpp LABELS core)
portside_add_test(tst_breaks core/tst_breaks.cpp LABELS core)
portside_add_test(tst_session core/tst_session.cpp LABELS core)
portside_add_test(tst_pacing core/tst_pacing.cpp LABELS core)
portside_add_test(tst_decoder emulation/tst_decoder.cpp LABELS emulation)
portside_add_test(tst_terminal emulation/tst_terminal.cpp LABELS emulation)
portside_add_test(tst_find_hex emulation/tst_find_hex.cpp LABELS emulation)
portside_add_test(tst_timestamps logging/tst_timestamps.cpp LABELS logging)
portside_add_test(tst_session_log logging/tst_session_log.cpp LABELS logging)
portside_add_test(tst_toml profiles/tst_toml.cpp LABELS profiles)
portside_add_test(tst_profiles profiles/tst_profiles.cpp LABELS profiles)
portside_add_test(tst_cli cli/tst_cli.cpp LIBS portside_cli_lib LABELS cli)
add_dependencies(tst_cli portside)
portside_add_test(tst_integration integration/tst_integration.cpp LIBS portside_cli_lib LABELS integration socat)
add_dependencies(tst_integration portside)

if(PORTSIDE_BUILD_GUI)
    portside_add_test(tst_gui_terminal_widget gui/tst_terminal_widget.cpp LIBS portside_gui_lib GUI)
    portside_add_test(tst_gui_session_view gui/tst_session_view.cpp LIBS portside_gui_lib GUI)
    portside_add_test(tst_gui_main_window gui/tst_main_window.cpp LIBS portside_gui_lib GUI)
    portside_add_test(tst_gui_dialogs gui/tst_dialogs.cpp LIBS portside_gui_lib GUI)
    portside_add_test(tst_gui_nonfunctional gui/tst_nonfunctional.cpp LIBS portside_gui_lib GUI)
    add_dependencies(tst_gui_nonfunctional portside)
endif()
