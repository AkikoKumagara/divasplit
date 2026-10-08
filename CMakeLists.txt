cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(divasplit C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

add_executable(${PROJECT_NAME} src/main.c)

target_link_libraries(${PROJECT_NAME}
  taihen_stub
  SceLibKernel_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  gcc
)

vita_create_self(${PROJECT_NAME}.suprx ${PROJECT_NAME}
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.yml
)

# Quick Menu toggles (SceShell side, needs QuickMenuReborn)
add_executable(divasplit_qm qm/src/main.c)
target_include_directories(divasplit_qm PRIVATE qm/include src)

target_link_libraries(divasplit_qm
  ${CMAKE_SOURCE_DIR}/qm/lib/libQuickMenuReborn_stub.a
  SceLibKernel_stub
  SceIofilemgr_stub
  gcc
)

vita_create_self(divasplit_qm.suprx divasplit_qm
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/qm/divasplit_qm.yml
)
