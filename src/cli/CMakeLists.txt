# The command line front end.  Its logic is a library so the tests can call
# the argument handling and formatting directly.
add_library(portside_cli_lib STATIC
    cli.h cli.cpp
    console.h console.cpp
    pass_through.h pass_through.cpp
)
target_link_libraries(portside_cli_lib
    PUBLIC portside_core
    PRIVATE portside_warnings)

add_executable(portside main.cpp)
target_link_libraries(portside PRIVATE portside_cli_lib portside_warnings)
