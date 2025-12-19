
# Add local CMake modules (e.g., FindPugiXML.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
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
  set(hdf5_target HDF5::HDF5) # config package
elseif(TARGET hdf5::hdf5)
  set(hdf5_target hdf5::hdf5) # FindHDF5 (3.19+) C target
elseif(DEFINED HDF5_LIBRARIES)
  set(hdf5_target ${HDF5_LIBRARIES}) # last-resort variables
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
find_package(ISMRMRD CONFIG REQUIRED) # <— important
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

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

add_executable(marshal src/marshal_main.cpp src/marshal_http.hpp src/marshal_ws.hpp src/marshal_state.hpp src/mrd_sink.cpp)
target_link_libraries(marshal PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json ${hdf5_target})


add_executable(playback services/playback/playback_main.cpp)
target_link_libraries(playback PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json ${ismrmrd_target})

add_executable(fk_client clients/fk_client/fk_client_main.cpp)
target_link_libraries(fk_client PRIVATE Boost::system Threads::Threads nlohmann_json::nlohmann_json)

set(ENABLE_VIZ_CLIENT ON)
if(ENABLE_VIZ_CLIENT)
  find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui viz)
  if(NOT OpenCV_FOUND)
    message(WARNING "OpenCV (with viz) not found -> disabling viz_client")
  else()
    message(STATUS "Found OpenCV: ${OpenCV_VERSION}") 
    if(OpenCV_INCLUDE_DIRS)
      list(REMOVE_DUPLICATES OpenCV_INCLUDE_DIRS) 
      message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}") 
    endif()
    add_executable(viz_client clients/viz_client/viz_client_main.cpp)
    if(TARGET OpenCV::opencv_core)
      target_link_libraries(viz_client PRIVATE
        Boost::system Threads::Threads nlohmann_json::nlohmann_json ${hdf5_target}
        OpenCV::opencv_core OpenCV::opencv_imgproc OpenCV::opencv_highgui OpenCV::opencv_viz)
    else()
      target_include_directories(viz_client PRIVATE ${OpenCV_INCLUDE_DIRS})
      target_link_libraries(viz_client PRIVATE
        Boost::system Threads::Threads nlohmann_json::nlohmann_json ${hdf5_target}
        ${OpenCV_LIBS})
    endif()
  endif()
else()
  message(STATUS "viz_client disabled (ENABLE_VIZ_CLIENT=OFF)")
endif()

add_executable(image_streamer clients/image_streamer/image_streamer_main.cpp)
target_link_libraries(image_streamer PRIVATE Boost::system Threads::Threads ${ismrmrd_target})

add_executable(mk_mrd src/mk_mrd.cpp)
target_link_libraries(mk_mrd PRIVATE ${ismrmrd_target} hdf5_serial)
target_compile_features(mk_mrd PRIVATE cxx_std_17)

# ---- WS producer example ----
add_executable(ws_producer
  clients/ws_producer/ws_producer.cpp
)
target_link_libraries(ws_producer PRIVATE Boost::system Threads::Threads)

# tools/make_image_message
add_executable(make_image_message tools/make_image_message.cpp)
target_link_libraries(make_image_message PRIVATE ${ismrmrd_target})

if(BUILD_TESTING)
  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    message(STATUS "Catch2 v3 not found; fetching upstream archive")
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)
  endif()
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

  add_executable(test_mrd_sink tests/test_mrd_sink.cpp src/mrd_sink.cpp)
  target_link_libraries(test_mrd_sink PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json ${hdf5_target})
  add_test(NAME test_mrd_sink COMMAND test_mrd_sink)

  add_executable(unit_http_handlers tests/test_http_handlers.cpp src/mrd_sink.cpp)
  target_link_libraries(unit_http_handlers PRIVATE Catch2::Catch2WithMain Boost::system Threads::Threads nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target})
  add_test(NAME unit_http_handlers COMMAND unit_http_handlers)

  add_executable(unit_ws_handlers tests/test_ws_handlers.cpp src/mrd_sink.cpp)
  target_link_libraries(unit_ws_handlers PRIVATE Catch2::Catch2WithMain Boost::system Threads::Threads nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target})
  add_test(NAME unit_ws_handlers COMMAND unit_ws_handlers)

  add_executable(unit_mk_mrd tests/test_mk_mrd.cpp)
  target_link_libraries(unit_mk_mrd PRIVATE Catch2::Catch2WithMain ${ismrmrd_target} ${hdf5_target})
  add_test(NAME unit_mk_mrd COMMAND unit_mk_mrd)

  add_executable(unit_make_image tests/test_make_image_message.cpp)
  target_link_libraries(unit_make_image PRIVATE Catch2::Catch2WithMain ${ismrmrd_target})
  add_test(NAME unit_make_image COMMAND unit_make_image)

  add_executable(unit_playback tests/test_playback.cpp)
  target_include_directories(unit_playback PRIVATE services/playback)
  target_link_libraries(unit_playback PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
  add_test(NAME unit_playback COMMAND unit_playback)
endif()

add_executable(fs_tail_client tools/fs_tail_client.cpp)
target_link_libraries(fs_tail_client PRIVATE nlohmann_json::nlohmann_json)
