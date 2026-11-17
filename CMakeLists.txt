cmake_minimum_required(VERSION 3.22)
project(gz_cesium VERSION 0.1.0 LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
set(_native_default "${CMAKE_CURRENT_SOURCE_DIR}/.deps/cesium")
if(DEFINED ENV{GZ_CESIUM_NATIVE_ROOT} AND NOT "$ENV{GZ_CESIUM_NATIVE_ROOT}" STREQUAL "")
  set(_native_default "$ENV{GZ_CESIUM_NATIVE_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/cesium/install/include/Cesium3DTilesSelection/Tileset.h")
  # Reuse the fleet workspace's existing dependency build when installed as
  # its src/gz_cesium submodule. Independent checkouts use .deps/cesium.
  set(_native_default "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/cesium")
endif()
set(GZ_CESIUM_NATIVE_ROOT "${_native_default}" CACHE PATH
    "Cesium Native build/dependency directory created by tools/build_cesium_native")
get_filename_component(GZ_CESIUM_NATIVE_ROOT "${GZ_CESIUM_NATIVE_ROOT}" ABSOLUTE)
set(GZ_CESIUM_VCPKG "${GZ_CESIUM_NATIVE_ROOT}/vcpkg/56bb2411609227288b70117ead2c47585ba07713/installed/x64-linux")
# These are private build dependencies; do not inject this vcpkg toolchain into
# the ROS/Gazebo build or let its static dependencies interpose Gazebo's ABI.
list(PREPEND CMAKE_PREFIX_PATH "${GZ_CESIUM_NATIVE_ROOT}/install" "${GZ_CESIUM_VCPKG}")
set(CESIUM_DISABLE_SPZ ON)
find_package(cesium-native CONFIG REQUIRED)

# Resolve Cesium's private fmt/spdlog targets before Gazebo's config files can
# select differently versioned system targets with those same CMake names.
# Gazebo itself keeps using its installed shared-library dependencies.
foreach(dep gz_sim_vendor gz_common_vendor gz_plugin_vendor gz_transport_vendor gz_msgs_vendor)
  find_package(${dep} REQUIRED)
endforeach()
find_package(gz-sim REQUIRED)
find_package(gz-common REQUIRED COMPONENTS graphics)
find_package(gz-plugin REQUIRED COMPONENTS register)
find_package(gz-transport REQUIRED)
find_package(gz-msgs REQUIRED)

add_library(gz_cesium_mesh STATIC src/mesh.cpp src/model.cpp)
set_target_properties(gz_cesium_mesh PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(gz_cesium_mesh PUBLIC cxx_std_20)
target_include_directories(gz_cesium_mesh PUBLIC include)
target_link_libraries(gz_cesium_mesh PUBLIC Cesium3DTilesSelection CesiumGltfContent gz-common::graphics gz-sim::gz-sim)
target_compile_options(gz_cesium_mesh PRIVATE -Wall -Wextra -Wpedantic)

add_library(gz_cesium_system SHARED src/system.cpp)
target_compile_features(gz_cesium_system PRIVATE cxx_std_20)
target_link_libraries(gz_cesium_system PRIVATE gz_cesium_mesh CesiumCurl
    gz-sim::gz-sim gz-plugin::register gz-transport::gz-transport gz-msgs::gz-msgs)
target_compile_options(gz_cesium_system PRIVATE -Wall -Wextra -Wpedantic)
if(UNIX AND NOT APPLE)
  target_link_options(gz_cesium_system PRIVATE "LINKER:--exclude-libs,ALL")
endif()
install(TARGETS gz_cesium_system LIBRARY DESTINATION lib)
add_executable(gz_cesium_probe src/probe.cpp)
target_compile_features(gz_cesium_probe PRIVATE cxx_std_20)
target_include_directories(gz_cesium_probe PRIVATE include)
target_link_libraries(gz_cesium_probe PRIVATE Cesium3DTilesSelection CesiumCurl)
install(TARGETS gz_cesium_probe RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY worlds fixtures config DESTINATION share/${PROJECT_NAME})
install(FILES README.md LICENSE DESTINATION share/${PROJECT_NAME})
install(PROGRAMS tools/cesium_markers.py tools/serve_assets.py DESTINATION lib/${PROJECT_NAME})
install(FILES tools/asset_transport.py DESTINATION lib/${PROJECT_NAME})
ament_environment_hooks(env-hooks/gz_cesium.dsv.in)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME gz_cesium_assets_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/test_asset_transport.py")
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(gz_cesium_mesh_test test/mesh_test.cpp)
  target_link_libraries(gz_cesium_mesh_test gz_cesium_mesh)
  target_compile_definitions(gz_cesium_mesh_test PRIVATE GZ_CESIUM_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_0.glb")
  if(UNIX AND NOT APPLE)
    target_link_options(gz_cesium_mesh_test PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
endif()
ament_package()
