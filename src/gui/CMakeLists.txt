# The desktop application.  Its widgets are a library so the GUI tests can
# drive them headless (QT_QPA_PLATFORM=offscreen).
add_library(portside_gui_lib STATIC
    palette.h palette.cpp
    styles.h styles.cpp
    keymap.h keymap.cpp
    terminal_widget.h terminal_widget.cpp
    signals_panel.h signals_panel.cpp
    device_list.h device_list.cpp
    session_controller.h session_controller.cpp
    session_view.h session_view.cpp
    settings_dialog.h settings_dialog.cpp
    profiles_dialog.h profiles_dialog.cpp
    transfer_dialog.h transfer_dialog.cpp
    main_window.h main_window.cpp
)
target_link_libraries(portside_gui_lib
    PUBLIC portside_core Qt6::Gui Qt6::Widgets
    PRIVATE portside_warnings)

add_executable(portside-gui WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(portside-gui PRIVATE portside_gui_lib portside_warnings)
set_target_properties(portside-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Portside"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.portside"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
