cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

project(polymarket_arb_detector LANGUAGES CXX)

include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PM_ALLOW_CONDA_BOOST "Allow Boost headers from Conda/Anaconda installs" ON)
set(PM_ALLOCATOR "system" CACHE STRING "Allocator override: system, mimalloc, jemalloc")
set_property(CACHE PM_ALLOCATOR PROPERTY STRINGS system mimalloc jemalloc)

set(_boost_system_include_candidates
    /usr/include
    /usr/local/include
)

set(_boost_conda_include_candidates)
foreach(_conda_prefix IN ITEMS
        "$ENV{CONDA_PREFIX}"
        "$ENV{HOME}/anaconda3"
        "$ENV{HOME}/miniconda3")
    if(_conda_prefix AND EXISTS "${_conda_prefix}/include/boost/beast.hpp")
        list(APPEND _boost_conda_include_candidates "${_conda_prefix}/include")
    endif()
endforeach()
list(REMOVE_DUPLICATES _boost_conda_include_candidates)

find_path(BOOST_INCLUDE_DIR
    NAMES boost/beast.hpp
    PATHS ${_boost_system_include_candidates}
    NO_DEFAULT_PATH
)

if(BOOST_INCLUDE_DIR)
    message(STATUS "Using Boost headers: ${BOOST_INCLUDE_DIR}")
elseif(PM_ALLOW_CONDA_BOOST)
    list(LENGTH _boost_conda_include_candidates _conda_boost_candidate_count)
    if(_conda_boost_candidate_count GREATER 0)
        list(GET _boost_conda_include_candidates 0 BOOST_INCLUDE_DIR)
        message(WARNING
            "Using Conda Boost headers from ${BOOST_INCLUDE_DIR}. "
            "This project uses Boost header-only and does not link libboost_*.")
    endif()
endif()

if(NOT BOOST_INCLUDE_DIR)
    message(FATAL_ERROR
        "Boost headers not found in system include paths. Install libboost-dev/libboost-system-dev, "
        "or rerun CMake with -DPM_ALLOW_CONDA_BOOST=ON to allow a Conda Boost install.")
endif()

# Boost.System header-only mode
add_definitions(-DBOOST_ERROR_CODE_HEADER_ONLY)
add_definitions(-DBOOST_ALL_NO_LIB)

include_directories(
    ${BOOST_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/deps
    ${CMAKE_SOURCE_DIR}/src
)

# Find system OpenSSL
find_package(OpenSSL REQUIRED)

set(_pm_allocator_library)
set(_pm_allocator_library_candidates
    /usr/lib
    /usr/local/lib
    "$ENV{CONDA_PREFIX}/lib"
    "$ENV{HOME}/anaconda3/lib"
    "$ENV{HOME}/miniconda3/lib"
)

if(PM_ALLOCATOR STREQUAL "mimalloc")
    find_library(_pm_allocator_library NAMES mimalloc PATHS ${_pm_allocator_library_candidates})
    if(NOT _pm_allocator_library)
        message(FATAL_ERROR "PM_ALLOCATOR=mimalloc requested but mimalloc was not found")
    endif()
elseif(PM_ALLOCATOR STREQUAL "jemalloc")
    find_library(_pm_allocator_library NAMES jemalloc PATHS ${_pm_allocator_library_candidates})
    if(NOT _pm_allocator_library)
        message(FATAL_ERROR "PM_ALLOCATOR=jemalloc requested but jemalloc was not found")
    endif()
elseif(NOT PM_ALLOCATOR STREQUAL "system")
    message(FATAL_ERROR "Unsupported PM_ALLOCATOR='${PM_ALLOCATOR}'")
endif()

# simdjson library — always optimized
add_library(simdjson STATIC deps/simdjson.cpp)
target_include_directories(simdjson PUBLIC deps)
target_compile_options(simdjson PRIVATE -O3 -march=native -DNDEBUG)

# Main executable
add_executable(arb_detector
    src/main.cpp
    src/runtime.cpp
    src/http_json.cpp
    src/websocket.cpp
    src/parser.cpp
    src/orderbook.cpp
    src/arbitrage.cpp
    src/logger.cpp
    src/market_metadata.cpp
    src/rewards.cpp
    src/oms.cpp
)

# Always -O3 -march=native for the hot path
target_compile_options(arb_detector PRIVATE -O3 -march=native -DNDEBUG)

# Deterministic unit test for the executor strategy + OMS (no network/keys).
add_executable(test_executor
    tests/test_executor.cpp
    src/rewards.cpp
    src/oms.cpp
)
target_include_directories(test_executor PRIVATE src deps)
target_compile_options(test_executor PRIVATE -O2)
target_link_libraries(test_executor pthread)

target_link_libraries(arb_detector
    simdjson
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

if(PM_ALLOCATOR STREQUAL "mimalloc")
    target_compile_definitions(arb_detector PRIVATE MI_MALLOC_OVERRIDE=1)
    target_link_libraries(arb_detector ${_pm_allocator_library})
    message(STATUS "Using allocator override: mimalloc (${_pm_allocator_library})")
elseif(PM_ALLOCATOR STREQUAL "jemalloc")
    target_link_libraries(arb_detector ${_pm_allocator_library})
    message(STATUS "Using allocator override: jemalloc (${_pm_allocator_library})")
else()
    message(STATUS "Using allocator override: system")
endif()

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(IPO_SUPPORTED)
    set_property(TARGET simdjson arb_detector PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO/LTO not enabled: ${IPO_ERROR}")
endif()

# Don't accidentally pull in anaconda runtime libs
set_target_properties(arb_detector PROPERTIES
    SKIP_BUILD_RPATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
)
