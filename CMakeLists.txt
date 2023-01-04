cmake_minimum_required (VERSION 3.8)

project ("vkcl-nbody")

find_package(Vulkan REQUIRED)
include_directories(${Vulkan_INCLUDE_DIR})

set(SOURCES
	vk_mem_alloc.cpp
	vkcl-nbody.cpp
	volk.c
)

add_executable (vkcl-nbody ${SOURCES})

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET vkcl-nbody PROPERTY CXX_STANDARD 20)
endif()
