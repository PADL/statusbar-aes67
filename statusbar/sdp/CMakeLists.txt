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
