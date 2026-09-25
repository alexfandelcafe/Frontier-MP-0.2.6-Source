cmake_minimum_required(VERSION 3.25)
project(FrontierMP VERSION 0.2.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FRONTIER_BUILD_CLIENT "Build the in-game client DLL" ON)
option(FRONTIER_BUILD_TOOLS "Build developer tools" ON)
option(FRONTIER_BUILD_TESTS "Build protocol unit tests" ON)
option(FRONTIER_BUILD_LAUNCHER "Build the Windows launcher/loader" ON)

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

if(WIN32 AND FRONTIER_BUILD_CLIENT)
    add_library(frontier_client SHARED
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
    target_include_directories(frontier_client PRIVATE shared/include client/include game/include)
    target_link_libraries(frontier_client PRIVATE frontier_shared ws2_32 version)
    frontier_enable_warnings(frontier_client)
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
                "$<TARGET_FILE_DIR:frontier_client>/game/content/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/game/content/ui/boot.sc.xml"
                "$<TARGET_FILE_DIR:frontier_client>/game/content/ui/boot.sc.xml"
        VERBATIM
    )
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
