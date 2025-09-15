cmake_minimum_required(VERSION 3.21)
project(data_marshal_beast LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# optional:
# set(CMAKE_C_STANDARD 11)
# set(CMAKE_C_STANDARD_REQUIRED ON)

# Boost
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

#HDF5
#find_package(HDF5 REQUIRED COMPONENTS C CXX HL)
find_package(HDF5 REQUIRED)
# Pick the best available imported target (config or find-module), else fall back
if(TARGET HDF5::HDF5)
  set(hdf5_target HDF5::HDF5)          # config package
elseif(TARGET hdf5::hdf5)
  set(hdf5_target hdf5::hdf5)          # FindHDF5 (3.19+) C target
elseif(DEFINED HDF5_LIBRARIES)
  set(hdf5_target ${HDF5_LIBRARIES})   # last-resort variables
else()
  message(FATAL_ERROR "No usable HDF5 target or libs found")
endif()


#include_directories(${HDF5_INCLUDE_DIRS})
#link_directories(${HDF5_LIBRARY_DIRS})
#message(STATUS "HDF5 include dirs: ${HDF5_INCLUDE_DIRS}")
#message(STATUS "HDF5 library dirs: ${HDF5_LIBRARY_DIRS}")
message(STATUS "HDF5 libraries: ${HDF5_LIBRARIES}")

# JSON
include(FetchContent)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# ISMRMRD (from /usr/local via CMAKE_PREFIX_PATH)
find_package(ISMRMRD CONFIG REQUIRED)  # <— important
link_directories(${ISMRMRD_LIBRARY_DIRS})
include_directories(${ISMRMRD_INCLUDE_DIRS}) # optional
message(STATUS "ISMRMRD include dirs: ${ISMRMRD_INCLUDE_DIRS}")
message(STATUS "ISMRMRD library dirs: ${ISMRMRD_LIBRARY_DIRS}")
message(STATUS "ISMRMRD libraries: ${ISMRMRD_LIBRARIES}")

# Normalize target name in case of environment differences (optional)
if(TARGET ISMRMRD::ISMRMRD)
  set(ismrmrd_target ISMRMRD::ISMRMRD)
elseif(TARGET ismrmrd::ismrmrd)
  set(ismrmrd_target ismrmrd::ismrmrd)
elseif(DEFINED ISMRMRD_LIBRARIES)
  set(ismrmrd_target ${ISMRMRD_LIBRARIES})
else()
  message(FATAL_ERROR "ISMRMRD found, but no usable CMake target or library variable was exported.")
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(marshal src/marshal_main.cpp src/marshal_http.hpp src/marshal_ws.hpp src/marshal_state.hpp)
target_link_libraries(marshal PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json)

add_executable(dumpbox services/dumpbox/dumpbox_main.cpp)
target_link_libraries(dumpbox PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json ${ismrmrd_target})

add_executable(playback services/playback/playback_main.cpp)
target_link_libraries(playback PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json ${ismrmrd_target})

add_executable(fk_client clients/fk_client/fk_client_main.cpp)
target_link_libraries(fk_client PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json)


add_executable(viz_client clients/viz_client/viz_client_main.cpp)
target_link_libraries(viz_client PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json)

add_executable(mk_mrd src/mk_mrd.cpp)
target_link_libraries(mk_mrd PRIVATE ${ismrmrd_target} hdf5_serial)
target_compile_features(mk_mrd PRIVATE cxx_std_17)

if(BUILD_TESTING)
enable_testing()
add_executable(unit_pose tests/test_pose_store.cpp src/marshal_state.hpp)
target_link_libraries(unit_pose PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
add_test(NAME unit_pose COMMAND unit_pose)


add_executable(it_http tests/test_http_endpoints.cpp src/marshal_state.hpp)
target_link_libraries(it_http PRIVATE Catch2::Catch2WithMain Boost::system Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME it_http COMMAND it_http)


add_executable(it_ws tests/test_ws_broadcast.cpp src/marshal_state.hpp)
target_link_libraries(it_ws PRIVATE Catch2::Catch2WithMain Boost::system Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME it_ws COMMAND it_ws)
endif()