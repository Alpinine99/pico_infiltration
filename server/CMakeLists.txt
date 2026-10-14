cmake_minimum_required(VERSION 3.23)

set(CMAKE_C_STANDARD 11)

project(pico-server C)

# logging
add_library(logger
  src/logger.c
)

target_include_directories(logger
  PRIVATE include
)

target_compile_definitions(logger
  PRIVATE NDEBUG=1
)

# db connector
add_library(db_connector
  src/db_connector.c
)

target_include_directories(db_connector
  PRIVATE include
  ../common/include
)

target_link_libraries(db_connector
  PRIVATE sqlite3
  logger
)

# control
add_library(control
  src/control.c
)

target_include_directories(control
  PRIVATE include
  ../common/include
)

target_link_libraries(control
  PRIVATE logger
)

add_subdirectory(src)
add_subdirectory(tests)

