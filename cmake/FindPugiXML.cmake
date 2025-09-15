
# FindPugiXML.cmake - lightweight finder creating PugiXML::PugiXML
# Tries CMake config from distro, then pkg-config, then headers/libs
find_package(pugixml CONFIG QUIET)
if (pugixml_FOUND)
  add_library(PugiXML::PugiXML INTERFACE IMPORTED)
  set_target_properties(PugiXML::PugiXML PROPERTIES
    INTERFACE_LINK_LIBRARIES pugixml
  )
  message(STATUS "Found pugixml via CONFIG")
  return()
endif()

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(PUGIXML QUIET pugixml)
endif()

find_path(PugiXML_INCLUDE_DIR NAMES pugixml.hpp
  HINTS ${PUGIXML_INCLUDE_DIRS} /usr/include /usr/local/include
)
find_library(PugiXML_LIBRARY NAMES pugixml
  HINTS ${PUGIXML_LIBRARY_DIRS} /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PugiXML DEFAULT_MSG PugiXML_LIBRARY PugiXML_INCLUDE_DIR)

if (PugiXML_FOUND)
  add_library(PugiXML::PugiXML UNKNOWN IMPORTED)
  set_target_properties(PugiXML::PugiXML PROPERTIES
    IMPORTED_LOCATION ${PugiXML_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${PugiXML_INCLUDE_DIR}
  )
endif()
