# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(storage-monitor VERSION 1.0.0 LANGUAGES CXX)

add_subdirectory(storage-monitor-agent)
add_subdirectory(btrfs-storage)
