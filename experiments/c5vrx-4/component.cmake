target_sources(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/pipeline.c")
target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE C5VRX4_EXPERIMENT=1)
target_bitscrambler_add_src("${PROJECT_DIR}/c5vrx4_span75.bsasm")
