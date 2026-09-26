cmake_minimum_required(VERSION 3.25)
project(FrontierMP VERSION 0.2.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FRONTIER_BUILD_CLIENT "Build the in-game client DLL" ON)
option(FRONTIER_BUILD_TOOLS "Build developer tools" ON)
option(FRONTIER_BUILD_TESTS "Build protocol unit tests" ON)
option(FRONTIER_BUILD_LAUNCHER "Build the Windows launcher/loader" ON)
set(FRONTIER_CEF_PACKAGE_DIR "" CACHE PATH "Optional packaged CEF runtime directory (for example client-rdrmp2/data/cef)")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

include(GNUInstallDirs)
include(cmake/FrontierWarnings.cmake)

add_library(frontier_shared STATIC
    shared/src/protocol.cpp
)
target_include_directories(frontier_shared PUBLIC shared/include)
frontier_enable_warnings(frontier_shared)

add_executable(frontier_server
    server/src/main.cpp
    server/src/server.cpp
    server/src/udp_socket.cpp
)
target_include_directories(frontier_server PRIVATE shared/include server/include)
target_link_libraries(frontier_server PRIVATE frontier_shared)
frontier_enable_warnings(frontier_server)

if(WIN32)
    target_link_libraries(frontier_server PRIVATE ws2_32)
endif()

add_executable(frontier_sim_client
    tools/sim_client/main.cpp
    client/src/network_client.cpp
    client/src/udp_socket.cpp
)
target_include_directories(frontier_sim_client PRIVATE shared/include client/include)
target_link_libraries(frontier_sim_client PRIVATE frontier_shared)
frontier_enable_warnings(frontier_sim_client)
if(WIN32)
    target_link_libraries(frontier_sim_client PRIVATE ws2_32)
endif()

if(FRONTIER_BUILD_LAUNCHER AND WIN32)
    add_executable(frontier_launcher
        launcher/src/main.cpp
        launcher/src/launcher.cpp
        game/src/build_fingerprint.cpp
    )
    target_include_directories(frontier_launcher PRIVATE shared/include game/include launcher/include)
    target_link_libraries(frontier_launcher PRIVATE version)
    frontier_enable_warnings(frontier_launcher)
    set_target_properties(frontier_launcher PROPERTIES OUTPUT_NAME "FrontierMP")
endif()

if(WIN32 AND FRONTIER_BUILD_TOOLS)
    add_executable(frontier_build_probe
        tools/build_probe/main.cpp
        game/src/build_fingerprint.cpp
    )
    target_include_directories(frontier_build_probe PRIVATE shared/include game/include)
    target_link_libraries(frontier_build_probe PRIVATE version)
    frontier_enable_warnings(frontier_build_probe)
endif()

if(WIN32 AND FRONTIER_CEF_PACKAGE_DIR)
    if(NOT EXISTS "${FRONTIER_CEF_PACKAGE_DIR}/include/cef_api_hash.h")
        message(FATAL_ERROR
            "FRONTIER_CEF_PACKAGE_DIR must point to a CEF distribution containing include/")
    endif()

    if(NOT EXISTS "${FRONTIER_CEF_PACKAGE_DIR}/libcef_dll_wrapper/CMakeLists.txt")
        message(FATAL_ERROR
            "FRONTIER_CEF_PACKAGE_DIR must include libcef_dll_wrapper/ from the CEF binary distribution")
    endif()

    find_library(FRONTIER_CEF_LIBRARY
        NAMES libcef
        PATHS
            "${FRONTIER_CEF_PACKAGE_DIR}/Release"
            "${FRONTIER_CEF_PACKAGE_DIR}/Debug"
        NO_DEFAULT_PATH
    )

    if(NOT FRONTIER_CEF_LIBRARY)
        message(FATAL_ERROR
            "Unable to find libcef.lib under FRONTIER_CEF_PACKAGE_DIR/Release or /Debug")
    endif()

    add_subdirectory(
        "${FRONTIER_CEF_PACKAGE_DIR}/libcef_dll_wrapper"
        "${CMAKE_CURRENT_BINARY_DIR}/frontier_cef_dll_wrapper"
        EXCLUDE_FROM_ALL
    )

    add_executable(frontier_cef_subprocess WIN32
        tools/cef_subprocess/main.cpp
    )
    target_include_directories(frontier_cef_subprocess PRIVATE
        "${FRONTIER_CEF_PACKAGE_DIR}"
    )
    target_link_libraries(frontier_cef_subprocess PRIVATE
        libcef_dll_wrapper
        "${FRONTIER_CEF_LIBRARY}"
    )
    frontier_enable_warnings(frontier_cef_subprocess)
    set_target_properties(frontier_cef_subprocess PROPERTIES
        OUTPUT_NAME "FrontierCefSubprocess"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )

    set(FRONTIER_CEF_OVERLAY_AVAILABLE ON)
else()
    set(FRONTIER_CEF_OVERLAY_AVAILABLE OFF)
endif()

if(WIN32 AND FRONTIER_BUILD_CLIENT)
    set(FRONTIER_CLIENT_SOURCES
        client/src/frontier_client.cpp
        client/src/client_runtime.cpp
        client/src/network_client.cpp
        client/src/interpolation.cpp
        client/src/remote_player_manager.cpp
        client/src/udp_socket.cpp
        game/src/build_fingerprint.cpp
        game/src/pattern_scanner.cpp
        game/src/rdr_bridge.cpp
        game/src/content_path_redirector.cpp
        game/src/native_invoker.cpp
        game/src/game_thread_dispatcher.cpp
        game/src/inline_hook.cpp
        game/src/startup_native_tracer.cpp
        game/src/frontier_session.cpp
    )

    if(FRONTIER_CEF_OVERLAY_AVAILABLE)
        list(APPEND FRONTIER_CLIENT_SOURCES
            client/src/cef_overlay.cpp
        )
    endif()

    add_library(frontier_client SHARED
        ${FRONTIER_CLIENT_SOURCES}
    )
    target_include_directories(frontier_client PRIVATE shared/include client/include game/include)
    target_link_libraries(frontier_client PRIVATE frontier_shared ws2_32 version)
    frontier_enable_warnings(frontier_client)

    if(FRONTIER_CEF_OVERLAY_AVAILABLE)
        target_include_directories(frontier_client PRIVATE
            "${FRONTIER_CEF_PACKAGE_DIR}"
        )
        target_link_libraries(frontier_client PRIVATE
            libcef_dll_wrapper
            "${FRONTIER_CEF_LIBRARY}"
        )
        target_compile_definitions(frontier_client PRIVATE
            FRONTIER_ENABLE_CEF=1
        )
        add_dependencies(frontier_client frontier_cef_subprocess)
    endif()

    set_target_properties(frontier_client PROPERTIES
        OUTPUT_NAME "FrontierClient"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )

    # Match the historical RDRMP distribution layout: the client package owns a
    # sibling ./game/content tree next to its DLL. FrontierClient's
    # fiAssetManager::fullReadPath hook resolves the historical content routes
    # into this external tree without modifying the user's RDR installation or
    # changing RDR.exe's working directory.
    add_custom_command(TARGET frontier_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:frontier_client>/game/content"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/game/content"
                "$<TARGET_FILE_DIR:frontier_client>/game/content"
        VERBATIM
    )
    
    # Historical RDRMP-compatible CEF package boundary. The UI source ships
    # with FrontierMP; an externally supplied CEF runtime can be overlaid at
    # build time without adding opaque Chromium binaries to the source tree.
    add_custom_command(TARGET frontier_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:frontier_client>/cef"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/cef"
                "$<TARGET_FILE_DIR:frontier_client>/cef"
        VERBATIM
    )

    if(FRONTIER_CEF_PACKAGE_DIR)
        add_custom_command(TARGET frontier_client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${FRONTIER_CEF_PACKAGE_DIR}"
                    "$<TARGET_FILE_DIR:frontier_client>/cef"
            VERBATIM
        )
    endif()
endif()

# The launcher and client are separate targets, but the launcher must never
# be considered runnable unless its DLL has been built and copied beside it.
if(WIN32 AND FRONTIER_BUILD_LAUNCHER AND FRONTIER_BUILD_CLIENT)
    add_dependencies(frontier_launcher frontier_client)
    add_custom_command(TARGET frontier_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:frontier_client>"
                "$<TARGET_FILE_DIR:frontier_launcher>/FrontierClient.dll"
        VERBATIM
    )
endif()

if(FRONTIER_BUILD_TESTS)
    enable_testing()
    add_executable(frontier_protocol_tests
        tests/protocol_tests.cpp
    )
    target_include_directories(frontier_protocol_tests PRIVATE shared/include)
    target_link_libraries(frontier_protocol_tests PRIVATE frontier_shared)
    frontier_enable_warnings(frontier_protocol_tests)
    add_test(NAME frontier_protocol_tests COMMAND frontier_protocol_tests)

    add_executable(frontier_pattern_tests
        tests/pattern_tests.cpp
        game/src/pattern_scanner.cpp
    )
    target_include_directories(frontier_pattern_tests PRIVATE game/include)
    frontier_enable_warnings(frontier_pattern_tests)
    add_test(NAME frontier_pattern_tests COMMAND frontier_pattern_tests)

    add_executable(frontier_interpolation_tests
        tests/interpolation_tests.cpp
        client/src/interpolation.cpp
    )
    target_include_directories(frontier_interpolation_tests PRIVATE shared/include client/include)
    target_link_libraries(frontier_interpolation_tests PRIVATE frontier_shared)
    frontier_enable_warnings(frontier_interpolation_tests)
    add_test(NAME frontier_interpolation_tests COMMAND frontier_interpolation_tests)
endif()
