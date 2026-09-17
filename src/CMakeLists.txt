add_library(quarry
  types.cpp
  encoding.cpp
  mapped_file.cpp
  segment.cpp
)
target_include_directories(quarry PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(quarry PUBLIC Threads::Threads PRIVATE quarry_flags)
