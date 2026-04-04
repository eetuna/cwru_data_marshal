cmake_minimum_required(VERSION 3.18)

# Support both standalone build and as a subdirectory of the main project
if(NOT CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL "crm_python")
    project(crm_python)
    set(CRM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
else()
    set(CRM_SRC_DIR ${PROJECT_SOURCE_DIR})
endif()

set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------
# Eigen5
# Eigen5 is a header-only library at ../eigen5 relative to the project
# root (i.e., a sibling of CRM_CPPImplementation). The include path is
# set to the parent of the eigen5 folder so that
# #include <eigen5/Eigen/Dense> resolves correctly -- matching the
# ../.. include path used in the Visual Studio projects.
# On systems where Eigen5 is installed as a CMake package (e.g., Linux),
# find_package will succeed and that installation takes precedence.
# ---------------------------------------------------------------

if(NOT TARGET Eigen5::Eigen)
    find_package(Eigen5 NO_MODULE QUIET)
    if(NOT Eigen5_FOUND)
        set(_eigen5_parent "${CRM_SRC_DIR}/..")
        if(NOT EXISTS "${_eigen5_parent}/eigen5/Eigen/Dense")
            message(FATAL_ERROR
                "Cannot find Eigen5 at ${_eigen5_parent}/eigen5.\n"
                "Please ensure the eigen5 directory is a sibling of CRM_CPPImplementation.")
        endif()
        add_library(Eigen5::Eigen INTERFACE IMPORTED GLOBAL)
        target_include_directories(Eigen5::Eigen INTERFACE "${_eigen5_parent}")
        message(STATUS "Eigen5 found at: ${_eigen5_parent}/eigen5")
    endif()
endif()

# ---------------------------------------------------------------
# Python and nanobind
# Python is found first so we can query it to locate nanobind.
# nanobind is installed via:  pip install nanobind
# ---------------------------------------------------------------

find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

# Auto-detect nanobind's CMake directory from the Python installation.
# This means no manual -Dnanobind_DIR= flag is ever needed.
if(NOT nanobind_DIR)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
        OUTPUT_VARIABLE _nb_cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _nb_result
    )
    if(_nb_result EQUAL 0 AND _nb_cmake_dir)
        set(nanobind_DIR "${_nb_cmake_dir}" CACHE PATH "nanobind CMake directory (auto-detected)")
    endif()
endif()
find_package(nanobind CONFIG REQUIRED)

# ---------------------------------------------------------------
# CRM source files
# ---------------------------------------------------------------

set(CRM_PY_SOURCES
    ${CRM_SRC_DIR}/src/CRM.hpp
    ${CRM_SRC_DIR}/src/CRM_MatrixOperations.hpp
    ${CRM_SRC_DIR}/src/CRM_StateVector_Definitions.hpp
    ${CRM_SRC_DIR}/src/CRM_BVPIVP_APIDeclarations.hpp
    ${CRM_SRC_DIR}/src/CRM_IVP_NumericalIntegrationTemplates.hpp
    ${CRM_SRC_DIR}/src/CRM_IVPSolver.cpp
    ${CRM_SRC_DIR}/src/CRM_BVPSolver.cpp
    ${CRM_SRC_DIR}/src/CRM_IVPJacobian.hpp
    ${CRM_SRC_DIR}/src/CRM_IVPJacobian_InternalAPI.hpp
    ${CRM_SRC_DIR}/src/CRM_IVPJacobian.cpp
    ${CRM_SRC_DIR}/src/CRM_FK_InternalAPI.hpp
    ${CRM_SRC_DIR}/src/CRM_ForwardKinematics.cpp
    ${CRM_SRC_DIR}/src/CRM_CatheterClass.cpp
    ${CRM_SRC_DIR}/src/CRM_SupportFunctions.cpp
    ${CRM_SRC_DIR}/numerical/minpack.hpp
    ${CRM_SRC_DIR}/numerical/minpack_TemplateDefinitions.hpp
    ${CRM_SRC_DIR}/numerical/minpack_wGivenJac_TemplateDefinitions.hpp
    ${CRM_SRC_DIR}/numerical/minpack.cpp
)

# ---------------------------------------------------------------
# Build the nanobind Python extension module
# ---------------------------------------------------------------

nanobind_add_module(crm
    crm_nanobind.cpp
    ${CRM_PY_SOURCES}
)

target_include_directories(crm PRIVATE
    ${CRM_SRC_DIR}/src
    ${CRM_SRC_DIR}/numerical
)

target_link_libraries(crm PRIVATE
    Eigen5::Eigen
)

# ---------------------------------------------------------------
# Install the module next to the Python sources
# ---------------------------------------------------------------

install(TARGETS crm LIBRARY DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})
