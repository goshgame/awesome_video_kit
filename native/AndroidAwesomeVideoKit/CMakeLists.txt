# 指定当前工程允许使用的最低 CMake 版本。
cmake_minimum_required(VERSION 3.22.1)
# 声明工程名称，并明确启用 C 与 C++ 两种语言。
project(AwesomeVideoKit LANGUAGES C CXX)

# 导出 compile_commands.json，方便 IDE、clangd 等工具做代码分析。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 设置 C 语言标准为 C11。
set(CMAKE_C_STANDARD 11)
# 要求编译器必须满足指定的 C 标准。
set(CMAKE_C_STANDARD_REQUIRED ON)
# 禁用编译器私有的 C 扩展，尽量保证可移植性。
set(CMAKE_C_EXTENSIONS OFF)
# 设置 C++ 标准为 C++17。
set(CMAKE_CXX_STANDARD 17)
# 要求编译器必须满足指定的 C++ 标准。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁用编译器私有的 C++ 扩展，尽量保证可移植性。
set(CMAKE_CXX_EXTENSIONS OFF)
# 为所有目标默认开启位置无关代码，便于生成共享库。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 单配置生成器下，按构建类型区分 Debug 与非 Debug 的体积优化策略。
set(AWESOME_ENABLE_SIZE_OPTIMIZATIONS ON)
set(AWESOME_ENABLE_POST_BUILD_STRIP ON)
if(CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug$")
    set(AWESOME_ENABLE_SIZE_OPTIMIZATIONS OFF)
    set(AWESOME_ENABLE_POST_BUILD_STRIP OFF)
endif()

# 记录项目根目录，后续统一基于它拼接相对路径。
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
# 记录第三方依赖库的根目录。
set(DEPENDENCY_LIBRARIES_DIR "${PROJECT_ROOT}/../DependencyLibraries")
# 记录 Android 与 iOS 共用的 FFmpeg C/C++ 业务源码目录。
set(SHARED_FFMPEG_TOOLS_DIR "${PROJECT_ROOT}/../shared/ffmpeg_tools")
# 记录当前 Android ABI；直接通过 CMake 调用时，默认回退到 arm64-v8a。
set(AWESOME_ANDROID_ABI "${ANDROID_ABI}")
if(NOT AWESOME_ANDROID_ABI)
    set(AWESOME_ANDROID_ABI "arm64-v8a")
endif()

if(AWESOME_ANDROID_ABI STREQUAL "arm64-v8a")
    set(AWESOME_DEFAULT_MBEDTLS_PREFIX "${DEPENDENCY_LIBRARIES_DIR}/mbedtls/mbedtls/out/android-arm64")
elseif(AWESOME_ANDROID_ABI STREQUAL "armeabi-v7a")
    set(AWESOME_DEFAULT_MBEDTLS_PREFIX "${DEPENDENCY_LIBRARIES_DIR}/mbedtls/mbedtls/out/android-armv7a")
else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI: ${AWESOME_ANDROID_ABI}")
endif()

set(AWESOME_DEFAULT_X264_PREFIX "${DEPENDENCY_LIBRARIES_DIR}/build/x264-android/android-${AWESOME_ANDROID_ABI}")
# FFmpeg 预编译产物所在目录前缀；优先读取按 ABI 拆分的新目录，兼容旧的平铺目录。
set(AWESOME_DEFAULT_FFMPEG_PREFIX "${PROJECT_ROOT}/jniLibs/${AWESOME_ANDROID_ABI}")
if(NOT EXISTS "${AWESOME_DEFAULT_FFMPEG_PREFIX}/lib")
    set(AWESOME_DEFAULT_FFMPEG_PREFIX "${PROJECT_ROOT}/jniLibs")
endif()
set(
    FFMPEG_PREFIX
    "${AWESOME_DEFAULT_FFMPEG_PREFIX}"
    CACHE PATH "FFmpeg Android install prefix (must contain include/ and lib/)"
)
# FFmpeg 头文件目录。
set(FFMPEG_INCLUDE_DIR "${FFMPEG_PREFIX}/include")
# FFmpeg 静态库目录。
set(FFMPEG_LIB_DIR "${FFMPEG_PREFIX}/lib")
# 定义需要链接的 FFmpeg 组件列表，后续用于批量导入与链接。
set(FFMPEG_COMPONENTS avformat avcodec avfilter avutil swresample swscale)

# 配置 mbedTLS 的安装前缀，并允许通过 CMake Cache 覆盖默认值。
set(
    MBEDTLS_PREFIX
    "${AWESOME_DEFAULT_MBEDTLS_PREFIX}"
    CACHE PATH "mbedTLS Android install prefix (must contain include/ and lib/)"
)
# 配置 x264 的安装前缀，并允许通过 CMake Cache 覆盖默认值。
set(
    X264_PREFIX
    "${AWESOME_DEFAULT_X264_PREFIX}"
    CACHE PATH "x264 Android install prefix (must contain lib/libx264.a or libx264.a)"
)

# 定义一个辅助函数：把已经存在的静态库文件注册成可链接的 IMPORTED target。
function(import_static_library target_name absolute_path)
    # 如果静态库文件不存在，直接终止配置阶段，避免后续链接时报错。
    if(NOT EXISTS "${absolute_path}")
        message(FATAL_ERROR "Static library not found for target `${target_name}`: ${absolute_path}")
    endif()

    # 将外部静态库声明为导入目标，供当前工程直接链接。
    add_library(${target_name} STATIC IMPORTED)
    # 指定该导入目标实际对应的静态库文件路径。
    set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${absolute_path}")
endfunction()

# 遍历所有 FFmpeg 组件，逐个导入对应的静态库。
foreach(component IN LISTS FFMPEG_COMPONENTS)
    import_static_library(${component} "${FFMPEG_LIB_DIR}/lib${component}.a")
endforeach()

# 定义需要链接的 mbedTLS 组件列表。
set(MBEDTLS_COMPONENTS mbedtls mbedx509 mbedcrypto)
# 遍历所有 mbedTLS 组件，逐个导入对应的静态库。
foreach(component IN LISTS MBEDTLS_COMPONENTS)
    import_static_library(${component} "${MBEDTLS_PREFIX}/lib/lib${component}.a")
endforeach()

# 单独导入 x264 静态库，兼容新分 ABI 输出目录和旧平铺源码目录。
set(X264_LIBRARY_PATH "${X264_PREFIX}/lib/libx264.a")
if(NOT EXISTS "${X264_LIBRARY_PATH}")
    set(X264_LIBRARY_PATH "${X264_PREFIX}/libx264.a")
endif()
import_static_library(x264 "${X264_LIBRARY_PATH}")

# 收集 shared/ffmpeg_tools 目录下的所有 C++ 源文件，并在文件变化时自动重新配置。
file(
    GLOB AwesomeVideoKit_FFMPEG_SOURCES
    CONFIGURE_DEPENDS
    "${SHARED_FFMPEG_TOOLS_DIR}/*.cpp"
)
# 对源文件列表排序，尽量保证构建输入顺序稳定。
list(SORT AwesomeVideoKit_FFMPEG_SOURCES)

# 如果没有找到任何业务源文件，则直接报错，避免生成空目标。
if(NOT AwesomeVideoKit_FFMPEG_SOURCES)
    message(FATAL_ERROR "No C++ source files were found in ${SHARED_FFMPEG_TOOLS_DIR}.")
endif()

# 声明最终输出的 Android 共享库目标。
add_library(AwesomeVideoKit SHARED)
# 为共享库目标指定全部源码，包括 C++ 业务源码和 JNI 入口文件。
target_sources(
    AwesomeVideoKit
    PRIVATE
    ${AwesomeVideoKit_FFMPEG_SOURCES}
    "${PROJECT_ROOT}/src/awesome_video_kit_manager_jni.c"
)

# 控制符号可见性，尽量隐藏内部实现符号，减小导出符号表。
set_target_properties(
    AwesomeVideoKit
    PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# 为目标添加头文件搜索路径，包含 FFmpeg、自身源码以及 mbedTLS 头文件。
target_include_directories(
    AwesomeVideoKit
    PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
    "${SHARED_FFMPEG_TOOLS_DIR}"
    "${MBEDTLS_PREFIX}/include"
)

# 非 Debug 构建开启按函数和数据分段，便于链接阶段移除未使用代码。
if(AWESOME_ENABLE_SIZE_OPTIMIZATIONS)
    target_compile_options(
        AwesomeVideoKit
        PRIVATE
        -ffunction-sections
        -fdata-sections
    )
endif()

# 始终隐藏静态库内部符号；仅在非 Debug 构建时启用段回收。
target_link_options(
    AwesomeVideoKit
    PRIVATE
    "-Wl,--exclude-libs,ALL"
    # 显式要求 16 KB ELF 段对齐，避免依赖某个 NDK 版本的默认行为。
    "-Wl,-z,max-page-size=16384"
)

if(AWESOME_ENABLE_SIZE_OPTIMIZATIONS)
    target_link_options(
        AwesomeVideoKit
        PRIVATE
        "-Wl,--gc-sections"
    )
endif()

# 链接 FFmpeg、mbedTLS、x264 以及 Android 平台所需的系统库。
target_link_libraries(
    AwesomeVideoKit
    PRIVATE
    ${FFMPEG_COMPONENTS}
    ${MBEDTLS_COMPONENTS}
    x264
    android
    atomic
    log
    m
    z
)

# 纯 CMake/Android Studio 构建时，Release/MinSizeRel 也自动 strip 最终 .so。
if(AWESOME_ENABLE_POST_BUILD_STRIP)
    if(CMAKE_STRIP)
        add_custom_command(
            TARGET AwesomeVideoKit
            POST_BUILD
            COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:AwesomeVideoKit>"
            COMMENT "Stripping AwesomeVideoKit for non-Debug build"
            VERBATIM
        )
    else()
        message(WARNING "CMAKE_STRIP is not set; non-Debug builds will not be stripped by CMake.")
    endif()
endif()
