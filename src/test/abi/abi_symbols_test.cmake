if(NOT DEFINED LIBRARY OR NOT EXISTS "${LIBRARY}")
  message(FATAL_ERROR "LIBRARY must point to the built renderer shared library")
endif()

if(NOT DEFINED NM OR NOT EXISTS "${NM}")
  message(FATAL_ERROR "NM must point to a working nm executable")
endif()

execute_process(
  COMMAND "${NM}" -D --defined-only "${LIBRARY}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error)

if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "failed to inspect exported symbols: ${nm_error}")
endif()

set(required_symbols
  we_session_create
  we_session_create_with_cache_path
  we_session_destroy
  we_session_set_source
  we_session_set_render_config
  we_session_resize_output
  we_session_set_user_properties_json
  we_session_apply_runtime_settings
  we_session_set_media_state
  we_session_push_audio_samples
  we_session_get_diagnostics_json
  we_session_play
  we_session_pause
  we_session_stop
  we_session_tick
  we_session_get_frame_ready_fd
  we_session_acquire_frame
  we_frame_release
  we_session_send_input_event)

foreach(symbol IN LISTS required_symbols)
  if(NOT nm_output MATCHES "(^|\n)[^\n]*[ \t]${symbol}(\n|$)")
    message(FATAL_ERROR "missing exported C ABI symbol: ${symbol}")
  endif()
endforeach()
