# db test
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

# control test
add_executable(control_test
  control_test.c
)

target_include_directories(control_test
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_link_libraries(control_test
  PRIVATE control
)

