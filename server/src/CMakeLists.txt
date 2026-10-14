add_executable(${PROJECT_NAME}
  main.c
)

target_include_directories(${PROJECT_NAME}
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE db_connector
  pthread
  control
  logger
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE _GNU_SOURCE
)

