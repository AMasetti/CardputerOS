# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/user/esp/esp-idf/components/bootloader/subproject"
  "/home/user/CardputerOS/os/build_qemu/bootloader"
  "/home/user/CardputerOS/os/build_qemu/bootloader-prefix"
  "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/tmp"
  "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/src/bootloader-stamp"
  "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/src"
  "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/user/CardputerOS/os/build_qemu/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
