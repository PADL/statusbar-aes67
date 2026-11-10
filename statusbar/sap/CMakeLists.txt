# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  sap
  SOURCES
  sap.cpp
  sap_receiver.cpp
  DEPS
  statusbar::buffer
  statusbar::ip
  statusbar::net
  statusbar::sdp
  statusbar::status
  TESTS
  sap_test.cpp)

# Fuzz harness (built only when -DENABLE_FUZZING=ON).
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/sap_directory_fuzzer.cpp)
