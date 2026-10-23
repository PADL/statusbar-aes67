# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  sap
  SOURCES
  sap.cpp
  sap_receiver.cpp
  DEPS
  statusbar::ip
  statusbar::net
  statusbar::sdp
  statusbar::status
  TESTS
  sap_test.cpp)
