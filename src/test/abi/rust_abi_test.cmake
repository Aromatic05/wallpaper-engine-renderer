if(NOT DEFINED RUSTC OR NOT EXISTS "${RUSTC}")
  message(FATAL_ERROR "rustc is required for the Rust ABI consumer test")
endif()
if(NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
  message(FATAL_ERROR "Rust ABI test source is missing: ${SOURCE}")
endif()
if(NOT DEFINED LIBRARY OR NOT EXISTS "${LIBRARY}")
  message(FATAL_ERROR "renderer library is missing: ${LIBRARY}")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "Rust ABI test output path was not provided")
endif()

get_filename_component(LIBRARY_DIR "${LIBRARY}" DIRECTORY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

execute_process(
  COMMAND "${RUSTC}"
          --edition=2021
          "${SOURCE}"
          -o "${OUTPUT}"
          "-Lnative=${LIBRARY_DIR}"
          -ldylib=wallpaper-engine-renderer
          "-Clink-arg=-Wl,-rpath,${LIBRARY_DIR}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_stdout
  ERROR_VARIABLE compile_stderr)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR
          "Rust ABI consumer failed to compile (${compile_result})\n${compile_stdout}\n${compile_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "LD_LIBRARY_PATH=${LIBRARY_DIR}:$ENV{LD_LIBRARY_PATH}"
          "${OUTPUT}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
          "Rust ABI consumer failed (${run_result})\n${run_stdout}\n${run_stderr}")
endif()
