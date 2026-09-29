cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum native macOS deployment version")
endif()
project(HaloMacOSSourceAOT LANGUAGES C OBJC)

option(BUILD_TESTING "Build host unit tests" OFF)
include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
pkg_get_variable(SDL3_INCLUDEDIR sdl3 includedir)

set(HALO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Halo source root")
set(HALO_AOT_DIR "${HALO_ROOT}/build/macos-aot" CACHE PATH "i386 guest ELF build directory")
set(HALO_LIFT_DIR "${HALO_AOT_DIR}/lifted-final" CACHE PATH "Generated ARM64 AOT C directory")
set(HALO_AOT_HOST_OPTIMIZATION "1" CACHE STRING "Native generated-C optimization level (0, 1, 2, 3, s); independent of guest i386 code generation")
set_property(CACHE HALO_AOT_HOST_OPTIMIZATION PROPERTY STRINGS 0 1 2 3 s)
if(NOT HALO_AOT_HOST_OPTIMIZATION MATCHES "^[0123s]$")
	message(FATAL_ERROR "HALO_AOT_HOST_OPTIMIZATION must be 0, 1, 2, 3, or s")
endif()
set(HALO_AOT_HOST_CPU "" CACHE STRING "Optional native generated-C CPU tuning, such as apple-m3; empty uses the toolchain default")
if(HALO_AOT_HOST_CPU)
	include(CheckCCompilerFlag)
	unset(HALO_AOT_HOST_CPU_SUPPORTED CACHE)
	check_c_compiler_flag("-mcpu=${HALO_AOT_HOST_CPU}" HALO_AOT_HOST_CPU_SUPPORTED)
	if(NOT HALO_AOT_HOST_CPU_SUPPORTED)
		message(FATAL_ERROR "Native compiler does not support -mcpu=${HALO_AOT_HOST_CPU}")
	endif()
endif()

# Compile upstream router discovery with the native ABI; no BSD structs cross into the guest.
file(GLOB HALO_MINIUPNPC_SOURCES CONFIGURE_DEPENDS "${HALO_ROOT}/port/third_party/miniupnpc/src/*.c")
add_library(halo_miniupnpc STATIC ${HALO_MINIUPNPC_SOURCES})
target_include_directories(halo_miniupnpc PUBLIC "${HALO_ROOT}/port/third_party/miniupnpc/include"
 PRIVATE "${HALO_ROOT}/port/third_party/miniupnpc/src")
target_compile_definitions(halo_miniupnpc PRIVATE MINIUPNP_STATICLIB MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR)

set(HALO_MACOS_HOST_SOURCES
	guest_address.c
	guest_allocator.c
	guest_call.c
	guest_image.c
	guest_heap.c
	guest_errno.c
	guest_x87_80.c
	recomp_state.c
	recomp_runtime.c
	guest_import_registry.c
	guest_callback.c
	guest_thread.c
	gl41_compat.c
	gl_bridge_support.c
	gl_host_imports.c
	gl_native_proc.c
	host_import_bindings.c
	host_imports.c
	host_imports_recomp.c
	host_memory_fingerprint.c
	host_game_evidence.c
	host_bink.c
	host_sdl.c
	host_app_icon.m
	host_sdl_audio.c
	host_services.c
	host_syscall.c
	posix_host_imports.c
	posix_backend_files.c
	posix_backend_net.c
	posix_backend_upnp.c
)

add_library(halo_macos_host STATIC ${HALO_MACOS_HOST_SOURCES})
target_compile_features(halo_macos_host PUBLIC c_std_11)
target_compile_definitions(halo_macos_host PUBLIC HALO_MACOS=1 RECOMP_GENERATED_CODE=1)
target_include_directories(halo_macos_host PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	${HALO_ROOT}/port/linux/src
	${HALO_AOT_DIR}
	${HALO_LIFT_DIR})
target_compile_options(halo_macos_host PRIVATE -Wall -Wextra -Werror)
target_link_libraries(halo_macos_host PUBLIC halo_miniupnpc PkgConfig::SDL3 PkgConfig::FFMPEG "-framework AppKit")

set(HALO_GENERATED_GL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-gl")
set(HALO_GENERATED_GL_C "${HALO_GENERATED_GL_DIR}/gl_wrappers.c")
set(HALO_GENERATED_GL_MANIFEST "${HALO_GENERATED_GL_DIR}/gl_tokens.json")
add_custom_command(
	OUTPUT "${HALO_GENERATED_GL_C}" "${HALO_GENERATED_GL_MANIFEST}"
	COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/macos_gl_bridge.py"
		--gl-header "${HALO_ROOT}/port/linux/src/gl.h"
		--sdl-header "${SDL3_INCLUDEDIR}/SDL3/SDL_opengl.h"
		--sdl-header "${SDL3_INCLUDEDIR}/SDL3/SDL_opengl_glext.h"
		--output "${HALO_GENERATED_GL_MANIFEST}"
		--c-output "${HALO_GENERATED_GL_C}"
	DEPENDS "${HALO_ROOT}/tools/macos_gl_bridge.py" "${HALO_ROOT}/port/linux/src/gl.h"
		"${SDL3_INCLUDEDIR}/SDL3/SDL_opengl.h" "${SDL3_INCLUDEDIR}/SDL3/SDL_opengl_glext.h"
	VERBATIM)
add_library(halo_macos_gl_wrappers OBJECT "${HALO_GENERATED_GL_C}")
target_compile_features(halo_macos_gl_wrappers PRIVATE c_std_11)
target_compile_definitions(halo_macos_gl_wrappers PRIVATE HALO_MACOS=1 RECOMP_GENERATED_CODE=1)
target_include_directories(halo_macos_gl_wrappers PRIVATE
	${HALO_LIFT_DIR} ${HALO_AOT_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(halo_macos_gl_wrappers PRIVATE PkgConfig::SDL3)

file(GLOB HALO_LIFT_SHARDS CONFIGURE_DEPENDS "${HALO_LIFT_DIR}/recomp_[0-9][0-9][0-9][0-9].c")
if(HALO_LIFT_SHARDS)
	add_library(halo_macos_guest OBJECT
		${HALO_LIFT_SHARDS}
		${HALO_LIFT_DIR}/recomp_dispatch.c
		${HALO_LIFT_DIR}/recomp_manual_dispatch.c
		${HALO_LIFT_DIR}/recomp_import_aliases.c
		${HALO_LIFT_DIR}/recomp_stubs_unresolved.c)
	target_compile_features(halo_macos_guest PRIVATE c_std_11)
	target_include_directories(halo_macos_guest PRIVATE ${HALO_LIFT_DIR} ${CMAKE_CURRENT_LIST_DIR})
	target_compile_options(halo_macos_guest PRIVATE "-O${HALO_AOT_HOST_OPTIMIZATION}" -Wno-parentheses-equality)
	if(HALO_AOT_HOST_CPU)
		target_compile_options(halo_macos_guest PRIVATE "-mcpu=${HALO_AOT_HOST_CPU}")
	endif()
	add_executable(halo_macos_aot main.c
		$<TARGET_OBJECTS:halo_macos_guest>
		$<TARGET_OBJECTS:halo_macos_gl_wrappers>)
	target_include_directories(halo_macos_aot PRIVATE ${HALO_LIFT_DIR} ${HALO_AOT_DIR})
	target_compile_features(halo_macos_aot PRIVATE c_std_11)
	target_compile_options(halo_macos_aot PRIVATE -Wall -Wextra -Werror)
	target_link_libraries(halo_macos_aot PRIVATE halo_macos_host PkgConfig::SDL3 m)
	# Archive members can be silently omitted if a guest alias satisfies the
	# same native symbol. Check even omitted members before the final link.
	add_custom_command(TARGET halo_macos_aot PRE_LINK
		COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/check_macos_symbol_namespaces.py"
			--lift-dir "${HALO_LIFT_DIR}"
			--archive "$<TARGET_FILE:halo_macos_host>"
			--archive "$<TARGET_FILE:halo_miniupnpc>"
		VERBATIM)
endif()

if(BUILD_TESTING)
	add_test(NAME halo_multiplayer_harness_guards
		COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/test_macos_multiplayer_harness.py")
	add_test(NAME halo_symbol_namespaces
		COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/test_macos_symbol_namespaces.py")
	add_executable(halo_gl41_copy_namespace_test gl41_compat.c tests/gl41_compat_test.c)
	target_compile_features(halo_gl41_copy_namespace_test PRIVATE c_std_11)
	target_compile_options(halo_gl41_copy_namespace_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_gl41_copy_namespace_test COMMAND halo_gl41_copy_namespace_test)
	add_executable(halo_gl_copy_bridge_test tests/gl_copy_bridge_test.c
		$<TARGET_OBJECTS:halo_macos_gl_wrappers>)
	target_link_libraries(halo_gl_copy_bridge_test PRIVATE halo_macos_host "-framework OpenGL")
	target_compile_options(halo_gl_copy_bridge_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_gl_copy_bridge_test COMMAND halo_gl_copy_bridge_test)
 add_executable(halo_input_bindings_test tests/input_bindings_test.c "${HALO_ROOT}/port/linux/src/input_bindings.c")
 target_link_libraries(halo_input_bindings_test PRIVATE PkgConfig::SDL3)
 target_compile_options(halo_input_bindings_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
 add_test(NAME halo_input_bindings_test COMMAND halo_input_bindings_test)

	add_executable(halo_host_bink_codec_test
		guest_address.c guest_call.c host_bink.c tests/host_bink_codec_test.c)
	target_include_directories(halo_host_bink_codec_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_host_bink_codec_test PRIVATE c_std_11)
	target_compile_options(halo_host_bink_codec_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_host_bink_codec_test PRIVATE PkgConfig::SDL3 PkgConfig::FFMPEG)
	# The fixture consumes user-owned movie data only when its path is supplied.
	set(HALO_BINK_TEST_MOVIE "" CACHE FILEPATH "Optional original intro.bik for native decoder verification")
	if(HALO_BINK_TEST_MOVIE)
		add_test(NAME halo_host_bink_codec_test COMMAND halo_host_bink_codec_test "${HALO_BINK_TEST_MOVIE}")
	endif()

	add_executable(halo_host_memory_fingerprint_test
		guest_address.c guest_call.c host_memory_fingerprint.c tests/host_memory_fingerprint_test.c)
	target_include_directories(halo_host_memory_fingerprint_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_host_memory_fingerprint_test PRIVATE c_std_11)
	target_compile_options(halo_host_memory_fingerprint_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_host_memory_fingerprint_test COMMAND halo_host_memory_fingerprint_test)
	add_executable(halo_gl_native_proc_cache_test gl_native_proc.c tests/gl_native_proc_cache_test.c)
	target_include_directories(halo_gl_native_proc_cache_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_options(halo_gl_native_proc_cache_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_gl_native_proc_cache_test PRIVATE PkgConfig::SDL3)
	add_test(NAME halo_gl_native_proc_cache_test COMMAND halo_gl_native_proc_cache_test)

	add_executable(halo_host_frame_metrics_test tests/host_frame_metrics_test.c)
	target_include_directories(halo_host_frame_metrics_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_host_frame_metrics_test PRIVATE c_std_11)
	target_compile_options(halo_host_frame_metrics_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_host_frame_metrics_test COMMAND halo_host_frame_metrics_test)

	add_executable(halo_gl_shader_source_test
		guest_address.c guest_allocator.c guest_call.c gl_bridge_support.c
		tests/gl_shader_source_test.c)
	target_include_directories(halo_gl_shader_source_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_gl_shader_source_test PRIVATE c_std_11)
	target_compile_options(halo_gl_shader_source_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_gl_shader_source_test PRIVATE PkgConfig::SDL3)
	add_test(NAME halo_gl_shader_source_test COMMAND halo_gl_shader_source_test)

	add_executable(halo_gl_bridge_wrapper_test
		guest_address.c guest_call.c tests/gl_bridge_wrapper_test.c
		"${HALO_GENERATED_GL_C}")
	target_include_directories(halo_gl_bridge_wrapper_test PRIVATE
			${HALO_LIFT_DIR} ${HALO_AOT_DIR} ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_gl_bridge_wrapper_test PRIVATE c_std_11)
	target_compile_definitions(halo_gl_bridge_wrapper_test PRIVATE
		HALO_MACOS=1 RECOMP_GENERATED_CODE=1)
	target_compile_options(halo_gl_bridge_wrapper_test PRIVATE
		-Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_gl_bridge_wrapper_test PRIVATE PkgConfig::SDL3)
	add_test(NAME halo_gl_bridge_wrapper_test COMMAND halo_gl_bridge_wrapper_test)

	add_executable(halo_posix_host_imports_test
		guest_address.c guest_call.c posix_host_imports.c
		posix_backend_files.c posix_backend_net.c posix_backend_upnp.c
		tests/posix_host_imports_test.c)
	target_include_directories(halo_posix_host_imports_test PRIVATE
		${CMAKE_CURRENT_LIST_DIR} ${HALO_ROOT}/port/linux/src)
	target_compile_features(halo_posix_host_imports_test PRIVATE c_std_11)
	target_compile_definitions(halo_posix_host_imports_test PRIVATE
		_POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE=1)
	target_compile_options(halo_posix_host_imports_test PRIVATE
		-Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_posix_host_imports_test PRIVATE halo_miniupnpc)
	add_test(NAME halo_posix_host_imports_test COMMAND halo_posix_host_imports_test)
 add_test(NAME halo_posix_local_alias_test COMMAND halo_posix_host_imports_test --alias)
 add_test(NAME halo_posix_alias_online_guard_test COMMAND halo_posix_host_imports_test --online)
 add_test(NAME halo_posix_alias_invalid_ip_test COMMAND halo_posix_host_imports_test --invalid-ip)
 add_test(NAME halo_posix_alias_collision_test COMMAND halo_posix_host_imports_test --collision)

	add_executable(halo_guest_image_test guest_address.c guest_image.c tests/guest_image_test.c)
	target_include_directories(halo_guest_image_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_guest_image_test PRIVATE c_std_11)
	target_compile_options(halo_guest_image_test PRIVATE -Wall -Wextra -Werror -pedantic -UNDEBUG)
	add_test(NAME halo_guest_image_test COMMAND halo_guest_image_test)

	add_executable(halo_memory_watch_fallback_test
		${HALO_ROOT}/port/linux/src/memory_watch.c
		tests/memory_watch_fallback_test.c)
	target_compile_definitions(halo_memory_watch_fallback_test PRIVATE HALO_MACOS=1 HALO_MEMORY_WATCH_TEST=1)
	target_include_directories(halo_memory_watch_fallback_test PRIVATE
		${CMAKE_CURRENT_LIST_DIR}/tests ${HALO_ROOT}/port/linux/src)
	target_compile_features(halo_memory_watch_fallback_test PRIVATE c_std_11)
	target_compile_options(halo_memory_watch_fallback_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_memory_watch_fallback_test COMMAND halo_memory_watch_fallback_test)

	add_executable(halo_guest_heap_test
		guest_address.c guest_allocator.c guest_call.c guest_heap.c
		host_syscall.c host_services.c tests/guest_heap_test.c)
	target_include_directories(halo_guest_heap_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_guest_heap_test PRIVATE c_std_11)
	target_compile_options(halo_guest_heap_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_guest_heap_test COMMAND halo_guest_heap_test)

	add_executable(halo_host_services_test
		guest_address.c guest_call.c host_services.c tests/host_services_test.c)
	add_executable(halo_host_syscall_test
		guest_address.c guest_allocator.c guest_call.c host_services.c
		host_syscall.c tests/host_syscall_test.c)
	foreach(clock_test halo_host_services_test halo_host_syscall_test)
		target_include_directories(${clock_test} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
		target_compile_features(${clock_test} PRIVATE c_std_11)
		target_compile_options(${clock_test} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
		add_test(NAME ${clock_test} COMMAND ${clock_test})
	endforeach()

	add_executable(halo_guest_x87_80_test
		guest_x87_80.c tests/guest_x87_80_test.c)
	target_include_directories(halo_guest_x87_80_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_guest_x87_80_test PRIVATE c_std_11)
	target_compile_options(halo_guest_x87_80_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_guest_x87_80_test COMMAND halo_guest_x87_80_test)

	add_executable(halo_guest_callback_test
		guest_address.c guest_allocator.c guest_call.c host_services.c
		guest_callback.c tests/guest_callback_test.c)
	target_include_directories(halo_guest_callback_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_guest_callback_test PRIVATE c_std_11)
	target_compile_options(halo_guest_callback_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_guest_callback_test PRIVATE PkgConfig::SDL3)
	add_test(NAME halo_guest_callback_test COMMAND halo_guest_callback_test)

	add_executable(halo_recomp_state_test recomp_state.c tests/recomp_state_test.c)
	target_include_directories(halo_recomp_state_test PRIVATE ${HALO_LIFT_DIR} ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_recomp_state_test PRIVATE c_std_11)
	target_compile_definitions(halo_recomp_state_test PRIVATE RECOMP_GENERATED_CODE=1)
	target_compile_options(halo_recomp_state_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME halo_recomp_state_test COMMAND halo_recomp_state_test)

	add_executable(halo_host_sdl_audio_test
		guest_address.c guest_allocator.c guest_call.c host_services.c
		guest_callback.c host_sdl_audio.c tests/host_sdl_audio_test.c)
	target_include_directories(halo_host_sdl_audio_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
	target_compile_features(halo_host_sdl_audio_test PRIVATE c_std_11)
	target_compile_definitions(halo_host_sdl_audio_test PRIVATE
		_POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE=1)
	target_compile_options(halo_host_sdl_audio_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	target_link_libraries(halo_host_sdl_audio_test PRIVATE PkgConfig::SDL3 Threads::Threads)
	add_test(NAME halo_host_sdl_audio_test COMMAND halo_host_sdl_audio_test)
	set_tests_properties(halo_host_sdl_audio_test PROPERTIES TIMEOUT 10)
endif()
