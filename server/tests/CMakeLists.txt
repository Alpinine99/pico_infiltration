add_executable(db_test
  db_test.c
)

target_include_directories(db_test
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_link_libraries(db_test
  PRIVATE db_connector
)

