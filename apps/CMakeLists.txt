add_executable(quarry_cli quarry_cli.cpp)
set_target_properties(quarry_cli PROPERTIES OUTPUT_NAME quarry)
target_link_libraries(quarry_cli PRIVATE quarry quarry_flags)
