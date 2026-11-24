cmake_minimum_required(VERSION 3.16)

# ASGEND native Qt 6 client (roadmap Q1). Standalone project: it does not
# depend on the C core build and can be configured on its own:
#   cmake -S clients/asgend_qt -B build-qt -DCMAKE_BUILD_TYPE=Release
project(asgend_qt
    VERSION 1.0.0
    DESCRIPTION "ASGEND native Qt 6 client"
    HOMEPAGE_URL "https://github.com/asgend/asgend"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASGEND_QT_BUILD_TESTS "Build Qt Test unit and integration tests" ON)
set(ASGEND_QT_USE_QTKEYCHAIN "AUTO" CACHE STRING
    "Use QtKeychain for credentials: AUTO (if found), ON (required), OFF")
set_property(CACHE ASGEND_QT_USE_QTKEYCHAIN PROPERTY STRINGS AUTO ON OFF)

# Reverse-DNS application id shared by the .desktop file, AppStream metainfo,
# icon and Flatpak manifest.
set(ASGEND_QT_APP_ID "org.asgend.QtClient")
set(ASGEND_QT_PACKAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/linux"
    CACHE PATH "Directory with Linux desktop integration files")

# Qt 6.4 is the oldest supported version (Ubuntu 24.04 LTS ships 6.4.2).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets Svg LinguistTools)
if(ASGEND_QT_BUILD_TESTS)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# ---------------------------------------------------------------- QtKeychain
set(ASGEND_QT_HAVE_QTKEYCHAIN OFF)
if(NOT ASGEND_QT_USE_QTKEYCHAIN STREQUAL "OFF")
    find_package(Qt6Keychain QUIET)
    if(Qt6Keychain_FOUND)
        set(ASGEND_QT_HAVE_QTKEYCHAIN ON)
    elseif(ASGEND_QT_USE_QTKEYCHAIN STREQUAL "ON")
        message(FATAL_ERROR "ASGEND_QT_USE_QTKEYCHAIN=ON but Qt6Keychain was not found")
    endif()
endif()
message(STATUS "ASGEND Qt client: QtKeychain support: ${ASGEND_QT_HAVE_QTKEYCHAIN}")

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------- protocol/core library
# This source list intentionally contains files present in this source release;
# UI features are kept in a single MainWindow until the view models are split.
add_library(asgendqt_core STATIC
    src/core/apierror.h src/core/apierror.cpp
    src/core/models.h src/core/models.cpp
    src/core/redact.h src/core/redact.cpp
    src/core/logging.h src/core/logging.cpp
    src/core/idempotency.h src/core/idempotency.cpp
    src/core/sha256verifier.h src/core/sha256verifier.cpp
    src/core/downloadsink.h src/core/downloadsink.cpp
    src/core/profile.h src/core/profile.cpp
    src/core/credentialstore.h src/core/credentialstore.cpp
    src/core/protocoladapter.h
    src/core/coreadapter.h src/core/coreadapter.cpp
    src/core/controlplaneadapter.h src/core/controlplaneadapter.cpp
    src/core/connectionmanager.h src/core/connectionmanager.cpp
)
target_include_directories(asgendqt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(asgendqt_core PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(asgendqt_core PUBLIC
    QT_NO_KEYWORDS
    ASGEND_QT_VERSION="${PROJECT_VERSION}"
    ASGEND_QT_APP_ID="${ASGEND_QT_APP_ID}")
if(ASGEND_QT_HAVE_QTKEYCHAIN)
    target_link_libraries(asgendqt_core PUBLIC Qt6Keychain::Qt6Keychain)
    target_compile_definitions(asgendqt_core PUBLIC ASGEND_QT_HAVE_QTKEYCHAIN=1)
endif()

# ------------------------------------------------------------------ UI library
add_library(asgendqt_ui STATIC
    src/ui/mainwindow.h src/ui/mainwindow.cpp
)
target_link_libraries(asgendqt_ui PUBLIC asgendqt_core Qt6::Widgets Qt6::Svg)

# App icon as an embedded resource, with a disk copy installed for desktops.
set(ASGEND_QT_ICON "${ASGEND_QT_PACKAGING_DIR}/${ASGEND_QT_APP_ID}.svg")
if(EXISTS "${ASGEND_QT_ICON}")
    set_source_files_properties("${ASGEND_QT_ICON}" PROPERTIES QT_RESOURCE_ALIAS "asgend-qt.svg")
    qt_add_resources(asgendqt_ui "asgendqt_icons" PREFIX "/icons" FILES "${ASGEND_QT_ICON}")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/translations/asgend_qt_ru.ts")
    qt_add_translations(asgendqt_ui TS_FILES translations/asgend_qt_ru.ts RESOURCE_PREFIX "/i18n")
endif()

# ------------------------------------------------------------------ executable
add_executable(asgend-qt src/main.cpp)
target_link_libraries(asgend-qt PRIVATE asgendqt_ui)
if(WIN32)
    set_target_properties(asgend-qt PROPERTIES WIN32_EXECUTABLE ON)
endif()

# ------------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS asgend-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES "${ASGEND_QT_PACKAGING_DIR}/${ASGEND_QT_APP_ID}.desktop"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES "${ASGEND_QT_ICON}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(FILES "${ASGEND_QT_PACKAGING_DIR}/${ASGEND_QT_APP_ID}.metainfo.xml"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

include("${ASGEND_QT_PACKAGING_DIR}/cpack.cmake" OPTIONAL)

# --------------------------------------------------------------------- tests
if(ASGEND_QT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
