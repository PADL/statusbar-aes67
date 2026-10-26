cmake_minimum_required(VERSION 3.31)
project(
  statusbar-aes67
  VERSION 0.1.0
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
enable_testing()

# Modules register bench tools into this aggregate target. An outer aggregate
# build supplies a single shared target instead.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  add_custom_target(run-all-benches)
endif()

# Dependency packages — must be installed and discoverable.
find_package(statusbar-core REQUIRED)

add_subdirectory(statusbar)

# fuzz-smoke and fuzz-all over the *_fuzzer targets (no-op unless ENABLE_FUZZING).
statusbar_register_fuzz_targets()

# Export each module target under the short statusbar::<name> form, so dependent
# packages find_package() them as statusbar::<module>.
set_target_properties(statusbar-mediaclock PROPERTIES EXPORT_NAME mediaclock)
set_target_properties(statusbar-rtp PROPERTIES EXPORT_NAME rtp)
set_target_properties(statusbar-sdp PROPERTIES EXPORT_NAME sdp)
set_target_properties(statusbar-sap PROPERTIES EXPORT_NAME sap)
set_target_properties(statusbar-aes67 PROPERTIES EXPORT_NAME aes67)

# Uniform statusbar- prefixed on-disk names for installed executables
# (statusbar_prefix_tool_names in cmake/module.cmake).
statusbar_prefix_tool_names()

# ---- install + export ----
# Standalone install/export boilerplate lives in
# statusbar_package_install_export (cmake/module.cmake); no-op under an outer
# aggregate build.
statusbar_package_install_export(NAME aes67)

# ---- packaging (CPack: TGZ + ZIP, plus DEB/RPM on Linux) ----
if(NOT STATUSBAR_AGGREGATE_BUILD)
  statusbar_package_cpack_defaults(
    NAME aes67 DESCRIPTION
    "AES67 — the PTP media clock, RTP and SDP for bridging.")
  set(CPACK_PACKAGE_VENDOR "PADL Software Pty Ltd")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Luke Howard <lukeh@padl.com>")
  set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
      "statusbar-aes67 (>= ${PROJECT_VERSION}), statusbar-core-dev (>= ${statusbar-core_VERSION})"
  )
  set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES
      "statusbar-aes67 >= ${PROJECT_VERSION}, statusbar-core-devel >= ${statusbar-core_VERSION}"
  )
  set(CPACK_COMPONENT_DEVEL_DESCRIPTION
      "C++ headers, static libraries and CMake package config")
  include(CPack)
endif()

# clang-tidy aggregate target (no-op unless clang-tidy is on PATH).
statusbar_register_clang_tidy_target()
