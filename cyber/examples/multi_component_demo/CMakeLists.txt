add_library(multi_component_demo SHARED
  multi_component_demo.cc
  ../proto/examples.pb.cc
)

target_link_libraries(multi_component_demo
  ${TARGET_NAME}
)

set_target_properties(multi_component_demo PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cyber/examples/multi_component_demo/"
)

file(COPY multi_component_demo.dag
  DESTINATION ${CMAKE_BINARY_DIR}/share/examples/multi_component_demo
)
