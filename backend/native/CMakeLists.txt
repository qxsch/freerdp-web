cmake_minimum_required(VERSION 3.16)
project(rdp_bridge VERSION 1.0.0 LANGUAGES C)

# Use C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build shared library
set(BUILD_SHARED_LIBS ON)

# Find FreeRDP3 packages
find_package(PkgConfig REQUIRED)

# Find FreeRDP3 packages
pkg_check_modules(FREERDP3 REQUIRED freerdp3)
pkg_check_modules(FREERDP_CLIENT3 REQUIRED freerdp-client3)
pkg_check_modules(WINPR3 REQUIRED winpr3)

# Create the shared library
add_library(rdp_bridge SHARED
    rdp_bridge.c
)

# Include directories
target_include_directories(rdp_bridge PRIVATE
    ${FREERDP3_INCLUDE_DIRS}
    ${FREERDP_CLIENT3_INCLUDE_DIRS}
    ${WINPR3_INCLUDE_DIRS}
)

# Link directories
target_link_directories(rdp_bridge PRIVATE
    ${FREERDP3_LIBRARY_DIRS}
    ${FREERDP_CLIENT3_LIBRARY_DIRS}
    ${WINPR3_LIBRARY_DIRS}
)

# Link libraries
target_link_libraries(rdp_bridge PRIVATE
    ${FREERDP3_LIBRARIES}
    ${FREERDP_CLIENT3_LIBRARIES}
    ${WINPR3_LIBRARIES}
    pthread
)

# Compiler flags
target_compile_options(rdp_bridge PRIVATE
    ${FREERDP3_CFLAGS_OTHER}
    ${FREERDP_CLIENT3_CFLAGS_OTHER}
    ${WINPR3_CFLAGS_OTHER}
    -Wall
    -Wextra
    -O2
)

# Set library properties
set_target_properties(rdp_bridge PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER rdp_bridge.h
)

# Install targets
install(TARGETS rdp_bridge
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

# Print configuration summary
message(STATUS "FreeRDP3 include dirs: ${FREERDP3_INCLUDE_DIRS}")
message(STATUS "FreeRDP3 libraries: ${FREERDP3_LIBRARIES}")
message(STATUS "WinPR3 libraries: ${WINPR3_LIBRARIES}")
