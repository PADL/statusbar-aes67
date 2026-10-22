# Package INTERFACE library, include path and the global accumulation properties
# (statusbar_package_module_init in cmake/module.cmake).
statusbar_package_module_init()

# Module subdirectories — dependency order.
add_subdirectory(mediaclock)
add_subdirectory(rtp)

# Populate the package INTERFACE library — skipped under an outer aggregate
# build, which populates a single shared `statusbar` itself.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  target_link_libraries(statusbar INTERFACE statusbar-mediaclock statusbar-rtp
                                            statusbar::test)
endif()

# Standalone per-package test binary (statusbar_package_test_binary in
# cmake/module.cmake); the outer aggregate build builds one combined
# statusbar_test from every aggregated package instead.
statusbar_package_test_binary()
