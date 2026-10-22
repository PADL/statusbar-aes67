# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  aes67
  SOURCES
  aes67_error.cpp
  aes67_receiver.cpp
  aes67_sender.cpp
  aes67_socket.cpp
  aes67_udp_transport.cpp
  aes67_xdp_transport.cpp
  DEPS
  statusbar::buffer
  statusbar::ieee
  statusbar::ip
  statusbar::mediaclock
  statusbar::net
  statusbar::rtp
  statusbar::sdp
  statusbar::status
  TESTS
  aes67_test.cpp
  aes67_xdp_test.cpp)
