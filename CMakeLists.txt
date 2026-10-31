# Needs MSYS2 CLANG64 (Clang, lld), the OpenXR SDK headers, CMake and Ninja:
#   pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld mingw-w64-clang-x86_64-clang-tools-extra mingw-w64-clang-x86_64-openxr-sdk mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja
# Add C:/msys64/clang64/bin to path
# handy commands for me (CMakePresets.json: "clang" builds in build)
#   cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="C:/Program Files (x86)/Steam/steamapps/common/NieRAutomata"
#   cmake --preset clang
#   cmake --build build
#   cmake --install build
#   cmake --build build --target launch
#   ctest --preset clang
#	-DNIERVR_VERSION=x
cmake_minimum_required(VERSION 3.20)
project(NieRVR LANGUAGES C CXX ASM)

option(NIERVR_TESTS "Build the unit tests" ON)
option(NIERVR_DEBUG_MODULE "Build NieRVR_debug.dll, the hot-reloaded debug tools (src/hot/)" ON)
# Shown in the menu, the stats overlay and the log. The release workflow passes the tag (v0.6.0 -> 0.6.0).
set(NIERVR_VERSION "dev" CACHE STRING "Version of this build")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # build/compile_commands.json, for clangd

include(FetchContent)
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/.deps)
FetchContent_Declare(minhook  # function hooking (upstream master, Sep 2026)
	URL https://github.com/TsudaKageyu/minhook/archive/8af6b4acae5a9388fd742b56fa79ece89d96f823.tar.gz
	URL_HASH SHA256=6c0334d09607da1c4a34a68b5febf0cf24c5810cae842eda13c9c24ea4196adb
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	SOURCE_SUBDIR none)
FetchContent_Declare(imgui    # Dear ImGui: the stats overlay (ui/stats_overlay.cpp)
	URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
	URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	SOURCE_SUBDIR none)
FetchContent_MakeAvailable(minhook imgui)  # sets minhook_SOURCE_DIR, imgui_SOURCE_DIR


option(NIERVR_DLSS "Download nvngx_dlss.dll (NVIDIA DLSS) at configure time and install it with the mod" OFF)
if(NIERVR_DLSS)
	set(DLSS_COMMIT 9981c6350dc58c0ce21f024fd65e31e851c31689)
	set(DLSS_DIR ${FETCHCONTENT_BASE_DIR}/dlss)
	file(DOWNLOAD https://github.com/NVIDIA/DLSS/raw/${DLSS_COMMIT}/lib/Windows_x86_64/rel/nvngx_dlss.dll
		${DLSS_DIR}/nvngx_dlss.dll
		EXPECTED_HASH SHA256=d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e)
	file(DOWNLOAD https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_COMMIT}/LICENSE.txt
		${DLSS_DIR}/LICENSE.txt
		EXPECTED_HASH SHA256=d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0)
endif()

file(GLOB_RECURSE NIERVR_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.S)
# Compiler warnings for the mod's own code (not ImGui's or MinHook's, which have their own).
# "missing field initializers" is off: brace-initialising a struct partly (= {a, b}) is used on purpose.
set(NIERVR_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)
set_source_files_properties(${NIERVR_SOURCES} PROPERTIES COMPILE_OPTIONS "${NIERVR_WARNINGS}")
list(FILTER NIERVR_SOURCES EXCLUDE REGEX "/src/(gen|hot)/")
add_library(nier_vr SHARED
	${NIERVR_SOURCES}
	src/gen/thunks.S
	${minhook_SOURCE_DIR}/src/buffer.c
	${minhook_SOURCE_DIR}/src/hook.c
	${minhook_SOURCE_DIR}/src/trampoline.c
	${minhook_SOURCE_DIR}/src/hde/hde64.c
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
	src/gen/exports.def)  # the export table: every d3d11.dll export, forwarded

set_target_properties(nier_vr PROPERTIES
	OUTPUT_NAME d3d11
	PREFIX ""                                      # d3d11.dll, not libd3d11.dll
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}   # build/d3d11.dll
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)

target_include_directories(nier_vr PRIVATE src ${minhook_SOURCE_DIR}/include ${imgui_SOURCE_DIR})
# windows.h without its rarely used parts and without min/max macros
target_compile_definitions(nier_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# ImGui only draws the overlay: no clipboard/IME/shell helpers (they'd import SHELL32 and more)
target_compile_definitions(nier_vr PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_WIN32_FUNCTIONS
	NIERVR_VERSION="${NIERVR_VERSION}")
# -fno-threadsafe-statics: a guarded function-local static pulls all of the C++ runtime's exception
# handling into the DLL (see build/d3d11.map); ImGui runs on one thread only. So a function-local
# static must not have a run-time initializer if two threads can reach it: keep those to constants.
target_compile_options(nier_vr PRIVATE
	-O2 -msse4.1 -fno-math-errno -fno-asynchronous-unwind-tables -fno-stack-protector
	$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
	
#statically link what we can so that its only one dll
target_link_options(nier_vr PRIVATE -static -Wl,--enable-stdcall-fixup -s
	-Wl,-Map=${CMAKE_BINARY_DIR}/d3d11.map)  

#dynamically linked libraries (included in windows)
target_link_libraries(nier_vr PRIVATE kernel32 user32 advapi32 winhttp bcrypt)

# NieRVR_debug.dll: the debug tools in src/hot/, reloaded by d3d11.dll whenever it's rebuilt (debug/hot_module.cpp)
if(NIERVR_DEBUG_MODULE)
file(GLOB NIERVR_HOT_SOURCES CONFIGURE_DEPENDS src/hot/*.cpp)
set_source_files_properties(${NIERVR_HOT_SOURCES} PROPERTIES COMPILE_OPTIONS "${NIERVR_WARNINGS}")
add_library(nier_vr_debug SHARED ${NIERVR_HOT_SOURCES})
set_target_properties(nier_vr_debug PROPERTIES
	OUTPUT_NAME NieRVR_debug
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF)
target_include_directories(nier_vr_debug PRIVATE src)
target_compile_definitions(nier_vr_debug PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(nier_vr_debug PRIVATE
	-O2 -msse4.1 -fno-math-errno -fno-asynchronous-unwind-tables -fno-stack-protector
	$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
target_link_options(nier_vr_debug PRIVATE -static -s)
target_link_libraries(nier_vr_debug PRIVATE kernel32 user32)
# straight into the game folder, so a build is all it takes to reload it
if(NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	add_custom_command(TARGET nier_vr_debug POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nier_vr_debug> ${CMAKE_INSTALL_PREFIX}
		VERBATIM)
endif()
install(TARGETS nier_vr_debug RUNTIME DESTINATION . COMPONENT debug)

# launch_debug also installs NieRVR_debug.dll (the hot-reloaded tools: turn Probe on)
add_custom_target(launch_debug
	COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
	COMMAND cmd /c start steam://rungameid/524220
	VERBATIM)
add_dependencies(launch_debug nier_vr nier_vr_debug)
endif()

# install target
install(TARGETS nier_vr RUNTIME DESTINATION . COMPONENT mod)
if(NIERVR_DLSS)
	install(FILES ${DLSS_DIR}/nvngx_dlss.dll DESTINATION . COMPONENT mod)
	install(FILES ${DLSS_DIR}/LICENSE.txt DESTINATION . RENAME nvngx_dlss_LICENSE.txt COMPONENT mod)
endif()

# launch target will isntall
add_custom_target(launch
	COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --component mod
	COMMAND cmd /c start steam://rungameid/524220
	VERBATIM)
add_dependencies(launch nier_vr)

# format targets
find_program(CLANG_FORMAT clang-format)
find_program(CLANG_TIDY clang-tidy)
file(GLOB NIERVR_STYLE_FILES CONFIGURE_DEPENDS src/*/*.cpp src/*/*.h tests/*.cpp)
list(FILTER NIERVR_STYLE_FILES EXCLUDE REGEX "/src/gen/")
if(CLANG_FORMAT)
	add_custom_target(format COMMAND ${CLANG_FORMAT} -i ${NIERVR_STYLE_FILES}
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMENT "clang-format: formatting src/ and tests/" VERBATIM)
	add_custom_target(format-check COMMAND ${CLANG_FORMAT} --dry-run --Werror ${NIERVR_STYLE_FILES}
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMENT "clang-format: checking src/ and tests/" VERBATIM)
endif()
if(CLANG_TIDY)
	set(NIERVR_TIDY_FILES ${NIERVR_STYLE_FILES})
	list(FILTER NIERVR_TIDY_FILES INCLUDE REGEX "\\.cpp$")
	add_custom_target(tidy COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet ${NIERVR_TIDY_FILES}
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMENT "clang-tidy: checking src/ and tests/" VERBATIM)
endif()

if(NIERVR_TESTS)
	enable_testing()
	file(GLOB NIERVR_TESTS_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
	foreach(test_src ${NIERVR_TESTS_SOURCES})
		get_filename_component(test_name ${test_src} NAME_WE)
		add_executable(${test_name} ${test_src})
		target_include_directories(${test_name} PRIVATE src)
		target_compile_options(${test_name} PRIVATE -msse4.1 ${NIERVR_WARNINGS})
		set_target_properties(${test_name} PROPERTIES
			CXX_STANDARD 17
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_test(NAME ${test_name} COMMAND ${test_name})
		set_tests_properties(${test_name} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
	endforeach()
endif()
