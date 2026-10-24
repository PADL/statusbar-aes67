# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  rtp
  SOURCES
  rtcp_ipmx.cpp
  rtcp_packet.cpp
  rtp_frame.cpp
  rtp_error.cpp
  rtp_header.cpp
  DEPS
  statusbar::buffer
  statusbar::ieee
  statusbar::ip
  statusbar::status
  TESTS
  rtcp_interval_test.cpp
  rtcp_ipmx_test.cpp
  rtcp_packet_test.cpp
  rtcp_time_test.cpp
  rtp_frame_test.cpp
  rtp_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON).
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/rtcp_parse_fuzzer.cpp)
