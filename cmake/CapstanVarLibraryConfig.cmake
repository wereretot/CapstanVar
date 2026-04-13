# CapstanVarLibraryConfig.cmake
# Find the CapstanVar tape warping library
#
# This config file is loaded by: find_package(CapstanVar REQUIRED)
#
# Defines:
#   CapstanVar::CapstanVarLib - The static library target

include(CMakeFindDependencyMacro)

# Load the exported targets
if(NOT TARGET CapstanVar::CapstanVarLib)
    include("${CMAKE_CURRENT_LIST_DIR}/CapstanVarLibraryTargets.cmake")
endif()

# Verify required components
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CapstanVar
    REQUIRED_VARS CapstanVarLib_FOUND
)
