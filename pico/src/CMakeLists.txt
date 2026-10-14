
if (DEFINED ENV{WIFI_SSID} AND (NOT DEFINED WIFI_SSID))
  set(WIFI_SSID $ENV{WIFI_SSID})
  set(WIFI_PASSWORD $ENV{WIFI_PASSWORD})
  message(":--> Using wifi ssid '${WIFI_SSID}'")
endif()

if (DEFINED ENV{SERVER_ADDRESS} AND (NOT DEFINED SERVER_ADDRESS))
  set(SERVER_ADDRESS $ENV{SERVER_ADDRESS})
  set(SERVER_PORT $ENV{SERVER_PORT})
  message(":--> Using server '${SERVER_ADDRESS}:${SERVER_PORT}'")
endif()

if (DEFINED ENV{ROOM_LOCATION} AND (NOT DEFINED ROOM_LOCATION))
  set(ROOM_LOCATION $ENV{ROOM_LOCATION})
  message(":--> Using ROOM_LOCATION '${ROOM_LOCATION}'")
endif()

if ((NOT WIFI_SSID) OR (NOT SERVER_ADDRESS) OR (NOT ROOM_LOCATION))
  message(FATAL_ERROR "Error make sure to set ENVs 'SERVER_ADDRESS' 'WIFI_SSID' 'ROOM_LOCATION'")
  return()
endif()

if (DEFINED ENV{NLOGGING} AND (NOT DEFINED NLOGGING))
  set(NLOGGING $ENV{NLOGGING})
elseif(NOT DEFINED NLOGGING)
  set(NLOGGING 0)
endif()
message(":--> Using NLOGGING '${NLOGGING}'")

if (DEFINED ENV{UPDATE_TIME_S} AND (NOT DEFINED UPDATE_TIME_S))
  set(UPDATE_TIME_S $ENV{UPDATE_TIME_S})
elseif(NOT DEFINED UPDATE_TIME_S)
  set(UPDATE_TIME_S 5)
endif()
message(":--> Using UPDATE_TIME_S '${UPDATE_TIME_S}'")

if (DEFINED ENV{REACTION_TIME_MS} AND (NOT DEFINED REACTION_TIME_MS))
  set(REACTION_TIME_MS $ENV{REACTION_TIME_MS})
elseif(NOT DEFINED REACTION_TIME_MS)
  set(REACTION_TIME_MS 500)
endif()
message(":--> Using REACTION_TIME_MS '${REACTION_TIME_MS}'")

if (DEFINED ENV{MAX_DIST_CM} AND (NOT DEFINED MAX_DIST_CM))
  set(MAX_DIST_CM $ENV{MAX_DIST_CM})
elseif(NOT DEFINED MAX_DIST_CM)
  set(MAX_DIST_CM 1000)
endif()
message(":--> Using MAX_DIST_CM '${MAX_DIST_CM}'")

if (DEFINED ENV{MAX_SENSORS} AND (NOT DEFINED MAX_SENSORS))
  set(MAX_SENSORS $ENV{MAX_SENSORS})
elseif(NOT DEFINED MAX_SENSORS)
  set(MAX_SENSORS 3)
endif()
message(":--> Using MAX_SENSORS '${MAX_SENSORS}'")

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
  PRIVATE NLOGGING=${NLOGGING}
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
  PRIVATE NLOGGING=${NLOGGING}
  PUBLIC HC_MAX_SENSORS=${MAX_SENSORS}
  HC_MAX_DIST_CM=${MAX_DIST_CM}
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
  conn_man
  HCSR04
)

target_include_directories(${PROJECT_NAME}
  PRIVATE ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/../common/include
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE NLOGGING=${NLOGGING}
  ROOM_LOCATION=\"${ROOM_LOCATION}\"
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  SERVER_ADDRESS=\"${SERVER_ADDRESS}\"
  SERVER_PORT=${SERVER_PORT}
  UPDATE_TIME_S=${UPDATE_TIME_S}
  REACTION_TIME_MS=${REACTION_TIME_MS}
)

if (${NLOGGING} EQUAL 0)
  pico_enable_stdio_usb(${PROJECT_NAME} 1)
else()
  pico_enable_stdio_usb(${PROJECT_NAME} 0)
endif()
pico_enable_stdio_uart(${PROJECT_NAME} 0)

pico_add_extra_outputs(${PROJECT_NAME})

