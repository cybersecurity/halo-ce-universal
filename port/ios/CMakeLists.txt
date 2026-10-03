# Build the iPhone/iPad host and SDL around the statically compiled guest image.
cmake_minimum_required(VERSION 3.24)
project(HaloCE LANGUAGES C CXX OBJC ASM)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    message(FATAL_ERROR "Browser-room integration currently targets iOS, not tvOS")
endif()
set(HALO_BUNDLE_IDENTIFIER "org.haloce.ios" CACHE STRING "App bundle identifier")
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/build/third_party/SDL3" sdl3 EXCLUDE_FROM_ALL)
set(HALO_PLATFORM_SOURCES Assets.xcassets host/host_import.m host/host_orientation.m host/host_touch.m)
set(HALO_INFO_PLIST Info.plist)
set(HALO_DEVICE_FAMILY "1,2")
add_executable(HaloCE MACOSX_BUNDLE
    ${HALO_PLATFORM_SOURCES} "${ROOT}/port/runtime/xiso.c"
    host/host_main.m host/host_memory.c host/host_loader.c host/host_syscall.c host/host_gl.c host/guest_call.S
    host/host_thread.c host/host_sdl.c network/host_room.m network/room_bridge.c
    "${ROOT}/port/web/src/web_net.c"
    "${ROOT}/port/linux/src/posix_files.c" host/posix_net.c
    "${ROOT}/build/ios/host/bridges.c" "${ROOT}/build/ios/embedded/guest_image.S")
target_include_directories(HaloCE PRIVATE host network "${ROOT}/port/web/src" "${ROOT}/port/runtime" "${ROOT}/port/runtime/include"
    "${ROOT}/port/runtime/guest/runtime" "${ROOT}/port/linux/src" "${ROOT}/build/ios/host"
    "${ROOT}/build/ios/gl_include" "${ROOT}/build/ios/embedded")
target_compile_definitions(HaloCE PRIVATE HALO_IOS=1 HALO_IOS_BROWSER_HOST=1 st_mtim=st_mtimespec st_atim=st_atimespec st_ctim=st_ctimespec)
target_compile_options(HaloCE PRIVATE -Wno-deprecated-declarations)
target_compile_options(HaloCE PRIVATE "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
target_link_libraries(HaloCE PRIVATE SDL3::SDL3-static "-framework WebKit" "-framework QuartzCore" "-framework Foundation" "-framework UIKit" "-framework OpenGLES")
target_link_libraries(HaloCE PRIVATE "-framework UniformTypeIdentifiers")
set_source_files_properties(Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(HaloCE PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
file(GLOB HALO_LICENSES "${ROOT}/build/ios/licenses/*.txt")
target_sources(HaloCE PRIVATE ${HALO_LICENSES})
set_source_files_properties(${HALO_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
set_target_properties(HaloCE PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_INFO_PLIST}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HALO_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${HALO_DEVICE_FAMILY}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
if(HALO_DEVELOPMENT_TEAM)
    set_target_properties(HaloCE PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${HALO_DEVELOPMENT_TEAM}")
endif()

# Ship the exact browser room protocol alongside its native packet adapter.
set(HALO_ROOM_SCRIPTS "${ROOT}/port/web/site/net.js" "${CMAKE_CURRENT_SOURCE_DIR}/network/room.js")
target_sources(HaloCE PRIVATE ${HALO_ROOM_SCRIPTS})
set_source_files_properties(${HALO_ROOM_SCRIPTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Network)
