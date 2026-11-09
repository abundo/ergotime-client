cmake_minimum_required(VERSION 3.16)
project(ergotime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ERGOTIME_STATIC "Link a static Qt (qtbase built with BUILD_SHARED_LIBS=OFF)" OFF)
set(QT_STATIC_PREFIX "$ENV{HOME}/Qt/static/6.12.0" CACHE PATH "Prefix from scripts/install-qt-static.sh")
if(ERGOTIME_STATIC)
    list(PREPEND CMAKE_PREFIX_PATH "${QT_STATIC_PREFIX}")
endif()

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets Sql Network)
find_package(Qt6 6.12 QUIET COMPONENTS DBus)

set(ERGOTIME_SOURCES
    src/main.cpp
    src/settings.cpp
    src/store.cpp
    src/idle.cpp
    src/tracker.cpp
    src/sync.cpp
    src/dialogs.cpp
    src/mainwindow.cpp
)

add_executable(ergotime ${ERGOTIME_SOURCES})
target_include_directories(ergotime PRIVATE src)
target_link_libraries(ergotime PRIVATE Qt6::Widgets Qt6::Sql Qt6::Network)

if(TARGET Qt6::DBus)
    target_link_libraries(ergotime PRIVATE Qt6::DBus)
    target_compile_definitions(ergotime PRIVATE ERGOTIME_HAS_DBUS=1)
endif()

if(WIN32)
    target_link_libraries(ergotime PRIVATE user32)
endif()

get_target_property(_qtcore_type Qt6::Core TYPE)
if(ERGOTIME_STATIC OR _qtcore_type STREQUAL "STATIC_LIBRARY")
    set(_static_plugins
        Qt6::QOffscreenIntegrationPlugin
        Qt6::QMinimalIntegrationPlugin
        Qt6::QSQLiteDriverPlugin
        Qt6::QTlsBackendCertOnlyPlugin
        Qt6::QTlsBackendOpenSSLPlugin
        Qt6::QTlsBackendSecureTransportPlugin
        Qt6::QComposePlatformInputContextPlugin)
    if(APPLE)
        list(APPEND _static_plugins Qt6::QCocoaIntegrationPlugin)
    elseif(WIN32)
        list(APPEND _static_plugins Qt6::QWindowsIntegrationPlugin)
    else()
        list(APPEND _static_plugins
            Qt6::QXcbIntegrationPlugin
            Qt6::QXcbGlxIntegrationPlugin
            Qt6::QXcbEglIntegrationPlugin
            Qt6::QWaylandIntegrationPlugin
            Qt6::QWaylandEglClientBufferPlugin
            Qt6::QWaylandXdgShellIntegrationPlugin
            Qt6::QWaylandWlShellIntegrationPlugin
            Qt6::QWaylandBradientDecorationPlugin)
    endif()
    set(_found_plugins)
    foreach(_plugin IN LISTS _static_plugins)
        if(TARGET ${_plugin})
            list(APPEND _found_plugins ${_plugin})
        endif()
    endforeach()
    if(_found_plugins)
        qt_import_plugins(ergotime INCLUDE ${_found_plugins})
    endif()
    if(UNIX AND NOT APPLE)
        target_link_options(ergotime PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_custom_command(TARGET ergotime POST_BUILD
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/pack-upx.sh" "$<TARGET_FILE:ergotime>"
        VERBATIM)
endif()

# The linux_gcc_64 kit links ICU from the same prefix (aqt archive "icu").
if(UNIX AND NOT APPLE AND NOT ERGOTIME_STATIC)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION)
    if(NOT _qtcore)
        get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
    endif()
    if(_qtcore)
        get_filename_component(_qtlib "${_qtcore}" DIRECTORY)
        set_target_properties(ergotime PROPERTIES
            BUILD_RPATH "${_qtlib}"
            INSTALL_RPATH "${_qtlib}")
    endif()
endif()

install(TARGETS ergotime RUNTIME DESTINATION bin)
