cmake_minimum_required(VERSION 3.22)

if (DEFINED ENV{VCPKG_ROOT})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else ()
	message(WARNING "VCPKG_ROOT is not set; continuing without vcpkg")
endif ()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "")
set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(
	ReticleCenter
	VERSION 0.1.0
	LANGUAGES CXX
)

# CommonLibF4 用 cxx_std_23；这里显式钉住，避免 compile-feature 传播没落到本 target
# 时 constinit / concepts 之类的 C++20 语法报 "syntax error"。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMAKE_SIZEOF_VOID_P 只有在 project() 之后才可用
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
	add_definitions(-D_AMD64_ -D_WIN64)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
	add_definitions(-D_X86_)
endif ()

find_package(minhook CONFIG REQUIRED)

if (NOT TARGET CommonLibF4)
	if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CommonLibF4")
		add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/CommonLibF4" CommonLibF4)
	elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibF4")
		add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibF4" CommonLibF4)
	else ()
		message(FATAL_ERROR "CommonLibF4 not found (looked in ./CommonLibF4 and ../CommonLibF4)")
	endif ()
endif ()

# ---- ImGui（源码编译，只带 dx11 + win32 后端）----
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
if (EXISTS ${IMGUI_DIR})
	add_library(imgui STATIC
		${IMGUI_DIR}/imgui.cpp
		${IMGUI_DIR}/imgui_draw.cpp
		${IMGUI_DIR}/imgui_tables.cpp
		${IMGUI_DIR}/imgui_widgets.cpp
		${IMGUI_DIR}/backends/imgui_impl_win32.cpp
		${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
	)
	target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
	target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
	set(RC_HAS_IMGUI ON)
else ()
	message(WARNING "imgui not found at ${IMGUI_DIR} -- building without the tuning menu")
	set(RC_HAS_IMGUI OFF)
endif ()

add_library(
	${PROJECT_NAME}
	SHARED
	src/main.cpp
	src/overlay.cpp
	src/imgui_ui.cpp
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${CMAKE_CURRENT_SOURCE_DIR}/CommonLibF4/CommonLibF4/include
		${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibF4/CommonLibF4/include
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		minhook::minhook
		CommonLibF4::CommonLibF4
)

if (RC_HAS_IMGUI)
	target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
	target_compile_definitions(${PROJECT_NAME} PRIVATE RC_HAS_IMGUI=1)
endif ()

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
		src/PCH.h
)

if (MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/sdl
			/utf-8
			/Zi
			/permissive-
			/Zc:preprocessor
			/W4
			/WX-
			"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
	)
endif ()
