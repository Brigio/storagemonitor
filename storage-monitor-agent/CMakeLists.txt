# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(storage-monitor-agent VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ============================================================
# Qt6
# ============================================================

find_package(Qt6 REQUIRED COMPONENTS
    Core
    DBus
    Sql
)

# ============================================================
# Resources
# ============================================================

qt6_add_resources(APP_RESOURCES
    resources.qrc
)

# ============================================================
# storage-monitor-agent
# ============================================================

add_executable(storage-monitor-agent

    main.cpp

    btrfsmonitor.cpp
    btrfsmonitor.h

    btrfseventmanager.cpp
    btrfseventmanager.h

    systemnotifier.cpp
    systemnotifier.h

    smartmonitor.cpp
    smartmonitor.h

    smarteventmanager.cpp
    smarteventmanager.h

    emailnotifier.cpp
    emailnotifier.h

    agentdbusservice.cpp
    agentdbusservice.h

    ${APP_RESOURCES}
    storage-monitor-agent.service
    README.md
    LICENSE
    org.storage_monitor.Agent.conf
    org.storage_monitor.Agent.policy

)

# ============================================================
# Qt libraries
# ============================================================

target_link_libraries(storage-monitor-agent PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Sql
)

include(GNUInstallDirs)

# ============================================================
# Install
# ============================================================

# /usr/bin/storage-monitor-agent
install(TARGETS storage-monitor-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# systemd unit
# /usr/lib/systemd/system/storage-monitor-agent.service
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/storage-monitor-agent.service DESTINATION lib/systemd/system)

# D-Bus system configuration
# /usr/share/dbus-1/system.d/org.storage_monitor.Agent.conf
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/org.storage_monitor.Agent.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)

# Polkit policy
# /usr/share/polkit-1/actions/org.storage_monitor.Agent.policy
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/org.storage_monitor.Agent.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
