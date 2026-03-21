add_library(WidthOpt SHARED WidthOpt.cpp)

target_include_directories(WidthOpt
  PRIVATE "${PROJECT_SOURCE_DIR}/include"
  SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(WidthOpt PRIVATE ${LLVM_DEFINITIONS})

target_link_libraries(
  WidthOpt
  "$<$<PLATFORM_ID:Darwin>:-undefined dynamic_lookup>"
)
