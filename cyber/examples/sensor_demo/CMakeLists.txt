protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS sensor_demo.proto)

add_library(sensor_component SHARED
  sensor_component.cc
  ${PROTO_SRCS}
)
target_include_directories(sensor_component PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(sensor_component ${TARGET_NAME})

add_library(control_component SHARED
  control_component.cc
  ${PROTO_SRCS}
)
target_include_directories(control_component PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(control_component ${TARGET_NAME})

add_executable(cmd_sender cmd_sender.cc ${PROTO_SRCS})
target_include_directories(cmd_sender PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cmd_sender ${TARGET_NAME})

foreach(tgt sensor_component control_component)
  set_target_properties(${tgt} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cyber/examples/sensor_demo/")
endforeach()

file(COPY sensor_demo.dag DESTINATION ${CMAKE_BINARY_DIR}/share/examples/sensor_demo)
