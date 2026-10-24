# Package INTERFACE library, include path and the global accumulation properties
# (statusbar_package_module_init in cmake/module.cmake).
statusbar_package_module_init()

# Helper: build a libFuzzer harness (Linux) or standalone driver (macOS) for the
# package's parsers. Mirrors avb's add_avb_fuzzer.
function(add_aes67_fuzzer source_file)
  if(NOT ENABLE_FUZZING)
    return()
  endif()
  get_filename_component(FUZZ_NAME ${source_file} NAME_WE)
  add_executable(${FUZZ_NAME} ${source_file})
  target_link_libraries(${FUZZ_NAME} PRIVATE statusbar)
  if(APPLE)
    target_compile_options(${FUZZ_NAME} PRIVATE -fsanitize=address,undefined
                                                -fno-omit-frame-pointer)
    target_link_options(${FUZZ_NAME} PRIVATE -fsanitize=address,undefined)
  else()
    target_compile_options(
      ${FUZZ_NAME} PRIVATE -fsanitize=fuzzer,address,undefined
                           -fno-omit-frame-pointer)
    target_link_options(
      ${FUZZ_NAME} PRIVATE -fsanitize=fuzzer,address,undefined
      ${FUZZER_EXTRA_LINK_OPTIONS})
  endif()
endfunction()

# Module subdirectories — dependency order.
add_subdirectory(mediaclock)
add_subdirectory(rtp)
add_subdirectory(sdp)
add_subdirectory(sap)
add_subdirectory(aes67)

# Populate the package INTERFACE library — skipped under an outer aggregate
# build, which populates a single shared `statusbar` itself.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  target_link_libraries(statusbar INTERFACE statusbar-mediaclock statusbar-rtp statusbar-sdp
                                            statusbar-sap statusbar-aes67
                                            statusbar::test)
endif()

# Standalone per-package test binary (statusbar_package_test_binary in
# cmake/module.cmake); the outer aggregate build builds one combined
# statusbar_test from every aggregated package instead.
statusbar_package_test_binary()
