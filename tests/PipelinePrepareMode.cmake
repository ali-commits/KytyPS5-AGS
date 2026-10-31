# A preparation request without game metadata must stop, never fall through to draws/gameplay.
set(ENV{KYTY_PIPELINE_LIST_PREPARE} 1)
set(ENV{KYTY_PIPELINE_LIST} 0)
set(ENV{KYTY_SHADER_PRECOMPILE} 0)
set(ENV{KYTY_PIPELINE_JOURNAL} 0)
set(ENV{KYTY_PIPELINE_FAST_FIRST} 0)
set(ENV{KYTY_PIPELINE_PREFETCH} 0)
execute_process(COMMAND "${FIXTURE}" --draw-prep-engine-only
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 40)
if(NOT "${result}" STREQUAL "2" OR NOT output MATCHES "KYTY_PREPARE_PROGRESS empty 0 0 1")
    message(FATAL_ERROR "Preparation did not stop for missing recipes/title metadata: ${result}\n${output}\n${errors}")
endif()
