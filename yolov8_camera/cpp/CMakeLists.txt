cmake_minimum_required(VERSION 3.10)

project(rknn_yolov8_camera_demo)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

set(rknpu_yolov8_file rknpu2/yolov8.cc)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/ utils.out)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

file(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

add_executable(${PROJECT_NAME}
    main.cc
    postprocess.cc
    ${rknpu_yolov8_file}
)
set(PKG_CONFIG_PATH $ENV{PKG_CONFIG_PATH})

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST_LIBRARIES REQUIRED
    gstreamer-1.0
    gstreamer-video-1.0
    glib-2.0
    gobject-2.0
    gstreamer-app-1.0
    
)
target_link_libraries(${PROJECT_NAME}
    ${GST_LIBRARIES}
    imageutils
    fileutils
    imagedrawing    
    ${LIBRKNNRT}
    dl
    /home/elf/aarch64-buildroot-linux-gnu_sdk-buildroot/aarch64-buildroot-linux-gnu/sysroot/usr/lib/libgstapp-1.0.so.0.2209.0
#     glib-2.0
#     gobject-2.0
    rknnrt
)
set(SYSROOT $ENV{SYSROOT})
message(STATUS " ${SYSROOT}/usr/include")
message(STATUS "!!!!!!!!!!!CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")


if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()
set(GST_INCLUDES 
    ${SYSROOT}/usr/include/gstreamer-1.0     # gst/gst.h所在目录的父级
    ${SYSROOT}/usr/include/glib-2.0          # GLib相关头文件
    ${SYSROOT}/usr/lib/glib-2.0/include      # GLib附加头文件
)
message(STATUS " ${GST_INCLUDES}")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
    ${GST_INCLUDES}

)

install(TARGETS ${PROJECT_NAME} DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/coco_80_labels_list.txt DESTINATION model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION model)