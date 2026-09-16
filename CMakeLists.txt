# Windows port of libaio (upstream baseline: pagure.io/libaio 0.3.113)
cmake_minimum_required(VERSION 3.15)
project(libaio C)

option(LIBAIO_BUILD_SHARED "Build the aio DLL" ON)
option(LIBAIO_BUILD_STATIC "Build the static aio_static library" ON)
option(LIBAIO_BUILD_TESTS  "Build test programs" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00
			NOMINMAX WIN32_LEAN_AND_MEAN
			_CRT_SECURE_NO_WARNINGS)

set(LIBAIO_SOURCES
	src/aio_backend.c
	src/io_cancel.c
	src/io_destroy.c
	src/io_getevents.c
	src/io_pgetevents.c
	src/io_queue_init.c
	src/io_queue_release.c
	src/io_queue_run.c
	src/io_queue_wait.c
	src/io_setup.c
	src/io_submit.c
)
set(LIBAIO_PUBLIC_HEADERS include/libaio.h)

if(LIBAIO_BUILD_SHARED)
	add_library(aio SHARED ${LIBAIO_SOURCES} ${LIBAIO_PUBLIC_HEADERS})
	target_compile_definitions(aio PRIVATE LIBAIO_BUILDING)
	target_include_directories(aio
		PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
			$<INSTALL_INTERFACE:include>
		PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	set_target_properties(aio PROPERTIES OUTPUT_NAME aio)
endif()

if(LIBAIO_BUILD_STATIC)
	add_library(aio_static STATIC ${LIBAIO_SOURCES})
	target_compile_definitions(aio_static PRIVATE LIBAIO_STATIC LIBAIO_BUILDING)
	target_include_directories(aio_static
		PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	set_target_properties(aio_static PROPERTIES OUTPUT_NAME aio_static)
endif()

if(LIBAIO_BUILD_TESTS)
	enable_testing()
	foreach(t test_basic test_deepspeed)
		add_executable(${t} tests/${t}.c)
		target_include_directories(${t} PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/include)
		if(LIBAIO_BUILD_SHARED)
			target_link_libraries(${t} PRIVATE aio)
		else()
			target_link_libraries(${t} PRIVATE aio_static)
		endif()
		add_test(NAME ${t} COMMAND ${t}
			 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
	endforeach()
endif()

include(GNUInstallDirs)
if(LIBAIO_BUILD_SHARED)
	install(TARGETS aio
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(LIBAIO_BUILD_STATIC)
	install(TARGETS aio_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES include/libaio.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
