# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(btrfs-storage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets Sql DBus)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Sql DBus)

# ============================================================
# DEFINIZIONE DEI SOURCES - MODIFICATO QUI
# ============================================================
set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        diskrowwidget.cpp
        btrfseventmanager.cpp
        btrfsmonitor.cpp
        btrfs_event_widget.cpp
        agentdbusclient.cpp
)

set(PROJECT_HEADERS
        mainwindow.h
        diskrowwidget.h
        diskinfo.h
        btrfseventmanager.h
        btrfsmonitor.h
        btrfs_event_widget.h
        agentdbusclient.h
)

set(PROJECT_FORMS
        mainwindow.ui
        diskrowwidget.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(btrfs-storage
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        ${PROJECT_HEADERS}
        ${PROJECT_FORMS}
        resources.qrc
        LICENSE
        btrfs.storage.desktop
    )
else()
    if(ANDROID)
        add_library(btrfs-storage SHARED
            ${PROJECT_SOURCES}
            ${PROJECT_HEADERS}
            ${PROJECT_FORMS}
        )
    else()
        add_executable(btrfs-storage
            ${PROJECT_SOURCES}
            ${PROJECT_HEADERS}
            ${PROJECT_FORMS}
        )
    endif()
endif()

target_link_libraries(btrfs-storage PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::DBus)

if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.btrfs-storage-main)
endif()
set_target_properties(btrfs-storage PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)

# 1. Installazione del binario principale
#    /usr/bin/btrfs-storage
install(TARGETS btrfs-storage
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Desktop launcher
install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/btrfs.storage.desktop
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/applications
)

# Application icon
install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/app.svg
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME
        btrfs-storage.svg
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(btrfs-storage)
endif()