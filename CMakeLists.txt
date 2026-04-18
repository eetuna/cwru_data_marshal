list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
cmake_minimum_required(VERSION 3.21)
project(data_marshal_beast LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------- Dependencies ----------

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

# HDF5
find_package(HDF5 REQUIRED)
if(TARGET HDF5::HDF5)
  set(hdf5_target HDF5::HDF5)
elseif(TARGET hdf5::hdf5)
  set(hdf5_target hdf5::hdf5)
elseif(DEFINED HDF5_LIBRARIES)
  set(hdf5_target ${HDF5_LIBRARIES})
else()
  message(FATAL_ERROR "No usable HDF5 target or libs found")
endif()
message(STATUS "HDF5 libraries: ${HDF5_LIBRARIES}")

# JSON
include(FetchContent)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# ISMRMRD
find_package(ISMRMRD CONFIG REQUIRED)
link_directories(${ISMRMRD_LIBRARY_DIRS})
include_directories(${ISMRMRD_INCLUDE_DIRS})
message(STATUS "ISMRMRD include dirs: ${ISMRMRD_INCLUDE_DIRS}")
message(STATUS "ISMRMRD library dirs: ${ISMRMRD_LIBRARY_DIRS}")
message(STATUS "ISMRMRD libraries: ${ISMRMRD_LIBRARIES}")

if(TARGET ISMRMRD::ISMRMRD)
  set(ismrmrd_target ISMRMRD::ISMRMRD)
elseif(TARGET ismrmrd::ismrmrd)
  set(ismrmrd_target ismrmrd::ismrmrd)
elseif(DEFINED ISMRMRD_LIBRARIES)
  set(ismrmrd_target ${ISMRMRD_LIBRARIES})
else()
  message(FATAL_ERROR "ISMRMRD found, but no usable CMake target.")
endif()

# libcurl (for recon forwarder and viz_client)
find_package(CURL REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# ---------- marshal server ----------

add_executable(marshal
  src/marshal_main.cpp
  src/mrd_sink.cpp
  src/dump_recorder.cpp
  src/live_image_recorder.cpp
  src/latest_image_writer.cpp
)
target_link_libraries(marshal PRIVATE
  Boost::system Threads::Threads nlohmann_json::nlohmann_json
  ${hdf5_target} ${ismrmrd_target} CURL::libcurl)

# ---------- kspace_streamer ----------

add_executable(kspace_streamer
  clients/kspace_streamer/kspace_streamer_main.cpp
  third_party/kissfft/kiss_fft.c
  third_party/kissfft/kiss_fftnd.c
)
target_include_directories(kspace_streamer PRIVATE third_party/kissfft)
target_link_libraries(kspace_streamer PRIVATE
  Boost::system Threads::Threads ${ismrmrd_target} CURL::libcurl)

# ---------- image_streamer ----------

add_executable(image_streamer clients/image_streamer/image_streamer_main.cpp)
target_link_libraries(image_streamer PRIVATE
  Boost::system Threads::Threads ${ismrmrd_target} CURL::libcurl)

# ---------- viz_client ----------

option(ENABLE_VIZ_CLIENT "Build viz_client (requires OpenCV)" ON)
if(ENABLE_VIZ_CLIENT)
  find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui)
  if(OpenCV_FOUND)
    message(STATUS "Found OpenCV: ${OpenCV_VERSION}")
    add_executable(viz_client clients/viz_client/viz_client_main.cpp)
    target_link_libraries(viz_client PRIVATE
      Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl
      ${ismrmrd_target})
    if(TARGET OpenCV::opencv_core)
      target_link_libraries(viz_client PRIVATE
        OpenCV::opencv_core OpenCV::opencv_imgproc OpenCV::opencv_highgui)
    else()
      target_include_directories(viz_client PRIVATE ${OpenCV_INCLUDE_DIRS})
      target_link_libraries(viz_client PRIVATE ${OpenCV_LIBS})
    endif()
  else()
    message(WARNING "OpenCV not found -> disabling viz_client")
  endif()
endif()

# ---------- Tests ----------

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

  add_executable(unit_pose tests/test_pose_store.cpp)
  target_link_libraries(unit_pose PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
  add_test(NAME unit_pose COMMAND unit_pose)

  add_executable(test_mrd_sink tests/test_mrd_sink.cpp src/mrd_sink.cpp)
  target_link_libraries(test_mrd_sink PRIVATE
    Catch2::Catch2WithMain nlohmann_json::nlohmann_json
    ${hdf5_target} ${ismrmrd_target})
  add_test(NAME test_mrd_sink COMMAND test_mrd_sink)

  add_executable(unit_http_handlers
    tests/test_http_handlers.cpp
    src/mrd_sink.cpp
    src/dump_recorder.cpp
    src/live_image_recorder.cpp
    src/latest_image_writer.cpp
  )
  target_link_libraries(unit_http_handlers PRIVATE
    Catch2::Catch2WithMain Boost::system Threads::Threads
    nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target} CURL::libcurl)
  add_test(NAME unit_http_handlers COMMAND unit_http_handlers)

  add_executable(it_http
    tests/test_http_endpoints.cpp
    src/mrd_sink.cpp
    src/dump_recorder.cpp
    src/live_image_recorder.cpp
    src/latest_image_writer.cpp
  )
  target_link_libraries(it_http PRIVATE
    Catch2::Catch2WithMain Boost::system Threads::Threads
    nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target} CURL::libcurl)
  add_test(NAME it_http COMMAND it_http)

  add_executable(test_ws_client
    tests/test_ws_client.cpp
    src/mrd_sink.cpp
    src/dump_recorder.cpp
    src/live_image_recorder.cpp
    src/latest_image_writer.cpp
  )
  target_link_libraries(test_ws_client PRIVATE
    Catch2::Catch2WithMain Boost::system Threads::Threads
    nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target} CURL::libcurl)
  add_test(NAME test_ws_client COMMAND test_ws_client)

  # HIGH #4 regression: scanner pushback must not block the recon reader.
  add_executable(test_scanner_pushback
    tests/test_scanner_pushback.cpp
    src/mrd_sink.cpp
    src/dump_recorder.cpp
    src/live_image_recorder.cpp
    src/latest_image_writer.cpp
  )
  target_link_libraries(test_scanner_pushback PRIVATE
    Catch2::Catch2WithMain Boost::system Threads::Threads
    nlohmann_json::nlohmann_json ${hdf5_target} ${ismrmrd_target} CURL::libcurl)
  add_test(NAME test_scanner_pushback COMMAND test_scanner_pushback)
endif()
