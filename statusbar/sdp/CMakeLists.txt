# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  sdp
  SOURCES
  sdp_error.cpp
  sdp_parser.cpp
  sdp_writer.cpp
  DEPS
  statusbar::ieee
  statusbar::ip
  statusbar::status
  TESTS
  sdp_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON).
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/sdp_parse_fuzzer.cpp)
