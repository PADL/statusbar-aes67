# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  rtp
  SOURCES
  rtp_error.cpp
  rtp_frame.cpp
  rtp_header.cpp
  rtp_rtcp_ipmx.cpp
  rtp_rtcp_packet.cpp
  DEPS
  statusbar::buffer
  statusbar::checksum
  statusbar::ieee
  statusbar::ip
  statusbar::status
  TESTS
  rtp_frame_test.cpp
  rtp_header_test.cpp
  rtp_rtcp_interval_test.cpp
  rtp_rtcp_ipmx_test.cpp
  rtp_rtcp_packet_test.cpp
  rtp_rtcp_time_test.cpp
  rtp_sequence_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON).
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/rtp_frame_parse_fuzzer.cpp)
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/rtp_parse_fuzzer.cpp)
add_aes67_fuzzer(${CMAKE_CURRENT_LIST_DIR}/rtp_rtcp_parse_fuzzer.cpp)
