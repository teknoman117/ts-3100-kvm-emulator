cmake_minimum_required(VERSION 3.14)
project(kvm-emulator VERSION 0.1 LANGUAGES C CXX)

option(DISASSEMBLE "Enable disassembly of client code as it executes" OFF)
option(VIRTUAL_DISK "Enable a virtual C: drive" OFF)

add_subdirectory(dependencies)
add_subdirectory(src)