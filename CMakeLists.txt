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

# Export each module target under the short statusbar::<name> form, so dependent
# packages find_package() them as statusbar::<module>.
set_target_properties(statusbar-mediabus PROPERTIES EXPORT_NAME mediabus)

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
    "AES67 — RTP, SDP, SAP and a wire-format media bus for bridging.")
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
