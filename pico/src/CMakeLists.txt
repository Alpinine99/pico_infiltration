
if (DEFINED ENV{WIFI_SSID} AND (NOT DEFINED WIFI_SSID))
  set(WIFI_SSID $ENV{WIFI_SSID})
  set(WIFI_PASSWORD $ENV{WIFI_PASSWORD})
  message("Using wifi ssid '${WIFI_SSID}'")
endif()

if (DEFINED ENV{SERVER_ADDRESS} AND (NOT DEFINED SERVER_ADDRESS))
  set(SERVER_ADDRESS $ENV{SERVER_ADDRESS})
  set(SERVER_PORT $ENV{SERVER_PORT})
  message("Using server '${SERVER_ADDRESS}:${SERVER_PORT}'")
endif()

if (DEFINED ENV{ROOM_LOCATION} AND (NOT DEFINED ROOM_LOCATION))
  set(ROOM_LOCATION $ENV{ROOM_LOCATION})
  message("Using ROOM_LOCATION '${ROOM_LOCATION}'")
endif()

if ((NOT WIFI_SSID) OR (NOT SERVER_ADDRESS) OR (NOT ROOM_LOCATION))
  message(FATAL_ERROR "Error make sure to set ENVs 'SERVER_ADDRESS' 'WIFI_SSID' 'ROOM_LOCATION'")
  return()
endif()

# Conn_man library
add_library(conn_man
  conn_man.cc
)

target_include_directories(conn_man
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_link_libraries(conn_man
  PRIVATE pico_stdlib
  pico_lwip_freertos
  pico_cyw43_arch_lwip_sys_freertos
  FreeRTOS-Kernel
)

target_compile_definitions(conn_man
  PRIVATE NLOGGING=1
)

# HCSR04 library
add_library(HCSR04
  HCSR04.c
)

target_include_directories(HCSR04
  PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(HCSR04
  PRIVATE pico_stdlib
  pico_lwip_freertos
  pico_cyw43_arch_lwip_sys_freertos
  FreeRTOS-Kernel
)

target_compile_definitions(HCSR04
  PRIVATE HC_MAX_SENSORS=3
  HC_MAX_DIST_CM=1000
  NLOGGING=1
)

# app compilation
add_executable(${PROJECT_NAME}
  main.cc
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE pico_stdlib
  pico_lwip_freertos 
  pico_cyw43_arch_lwip_sys_freertos
  hardware_adc
  hardware_watchdog
  FreeRTOS-Kernel
  FreeRTOS-Kernel-Heap4
  HCSR04
  conn_man
)

target_include_directories(${PROJECT_NAME}
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE NLOGGING=1
  ROOM_LOCATION=\"${ROOM_LOCATION}\"
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  SERVER_ADDRESS=\"${SERVER_ADDRESS}\"
  SERVER_PORT=${SERVER_PORT}
)

pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

pico_add_extra_outputs(${PROJECT_NAME})

