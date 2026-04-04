cmake_minimum_required(VERSION 3.18)
project(CRM_CPPImplementation)

# ---------------------------------------------------------------
# Eigen5
# Eigen5 is a header-only library at ../eigen5 relative to this project
# (i.e., a sibling of CRM_CPPImplementation). The include path is set to
# the parent of the eigen5 folder so that #include <eigen5/Eigen/Dense>
# resolves correctly -- matching the ../.. include path in the VS projects.
# On systems where Eigen5 is installed as a CMake package (e.g., Linux),
# find_package will succeed and that installation takes precedence.
# ---------------------------------------------------------------

if(NOT TARGET Eigen5::Eigen)
    find_package(Eigen5 NO_MODULE QUIET)
    if(NOT Eigen5_FOUND)
        set(_eigen5_parent "${CMAKE_CURRENT_SOURCE_DIR}/..")
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

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE ON)

set(CMAKE_CXX_STANDARD 17)

set(CRMSOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_MatrixOperations.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_StateVector_Definitions.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_BVPIVP_APIDeclarations.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_IVP_NumericalIntegrationTemplates.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_IVPSolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_BVPSolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_IVPJacobian.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_IVPJacobian_InternalAPI.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_IVPJacobian.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_FK_InternalAPI.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_ForwardKinematics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_CatheterClass.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CRM_SupportFunctions.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/numerical/minpack.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/numerical/minpack_TemplateDefinitions.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/numerical/minpack_wGivenJac_TemplateDefinitions.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/numerical/minpack.cpp
	)

add_library(CRMCPPLib STATIC
	${CRMSOURCES}
	)

target_include_directories(CRMCPPLib PUBLIC
	"${PROJECT_SOURCE_DIR}/src"
	"${PROJECT_SOURCE_DIR}/numerical"
        )

add_executable(CRMTest
	CRMCPPTest/CRMTest.cpp
	CRMCPPTest/CRMTest.h
	CRMCPPTest/CRM_KinematicsTestFunctions.hpp
	CRMCPPTest/CRM_KinematicsTestFunctions.cpp
	${CRMSOURCES}
	)

target_link_libraries(CRMTest PUBLIC
	Eigen5::Eigen
	)

target_include_directories(CRMTest PUBLIC
	"${PROJECT_SOURCE_DIR}/src"
	"${PROJECT_SOURCE_DIR}/numerical"
		)

# ---------------------------------------------------------------
# Optional: Python bindings (nanobind)
#   Enable with -DBUILD_PYTHON_BINDINGS=ON
# ---------------------------------------------------------------
option(BUILD_PYTHON_BINDINGS "Build Python bindings using nanobind" OFF)
if(BUILD_PYTHON_BINDINGS)
	add_subdirectory(python)
endif()

