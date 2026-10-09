cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(divasplit C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -Wshadow -O2 -std=gnu99")

# Profiler kernel plugin (loaded at boot, idle until divasplit starts it)
add_executable(divasplit_prof prof/src/main.c)
target_link_options(divasplit_prof PRIVATE -nostdlib)
target_link_libraries(divasplit_prof
  taihenModuleUtils_stub
  SceThreadmgrForDriver_stub
  SceSysmemForDriver_stub
  SceSysclibForDriver_stub
  SceIofilemgrForDriver_stub
  gcc
)
unset(VITA_ELF_CREATE_FLAGS)
unset(VITA_MAKE_FSELF_FLAGS)
vita_create_self(divasplit_prof.skprx divasplit_prof
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/prof/divasplit_prof.yml
)
vita_create_stubs(prof_stubs divasplit_prof ${CMAKE_SOURCE_DIR}/prof/divasplit_prof.yml KERNEL)

add_executable(${PROJECT_NAME} src/main.c src/threadview.c src/opt.c src/trace.c)
target_link_options(${PROJECT_NAME} PRIVATE -nostdlib)
add_dependencies(${PROJECT_NAME} prof_stubs)

target_link_libraries(${PROJECT_NAME}
  ${CMAKE_BINARY_DIR}/libdivasplit_prof_stub_weak.a
  taihen_stub
  SceGxm_stub
  SceSysmem_stub
  SceLibKernel_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  gcc
)

# vita.cmake appends to these in the caller's scope: start each module clean
unset(VITA_ELF_CREATE_FLAGS)
unset(VITA_MAKE_FSELF_FLAGS)
vita_create_self(${PROJECT_NAME}.suprx ${PROJECT_NAME}
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.yml
)

# Quick Menu toggles (SceShell side, needs QuickMenuReborn)
add_executable(divasplit_qm qm/src/main.c)
target_link_options(divasplit_qm PRIVATE -nostdlib)
target_include_directories(divasplit_qm PRIVATE qm/include src)

target_link_libraries(divasplit_qm
  ${CMAKE_SOURCE_DIR}/qm/lib/libQuickMenuReborn_stub.a
  SceLibKernel_stub
  SceIofilemgr_stub
  gcc
)

# vita.cmake appends to these in the caller's scope: start each module clean
unset(VITA_ELF_CREATE_FLAGS)
unset(VITA_MAKE_FSELF_FLAGS)
vita_create_self(divasplit_qm.suprx divasplit_qm
  UNSAFE
  CONFIG ${CMAKE_SOURCE_DIR}/qm/divasplit_qm.yml
)

# Installer app: installs / updates everything above and the VitaGrafix blocks
add_executable(divasplit_installer installer/src/main.c installer/src/edit.c)
target_include_directories(divasplit_installer PRIVATE src installer/src)

target_link_libraries(divasplit_installer
  SceDisplay_stub
  SceCtrl_stub
  ScePower_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
)

# vita.cmake appends to these in the caller's scope: start each module clean
unset(VITA_ELF_CREATE_FLAGS)
unset(VITA_MAKE_FSELF_FLAGS)
vita_create_self(eboot.bin divasplit_installer UNSAFE)
vita_create_vpk(divasplit_installer.vpk DVSP00001 eboot.bin
  VERSION 01.09
  NAME "divasplit installer"
  FILE installer/sce_sys/icon0.png sce_sys/icon0.png
  FILE installer/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE installer/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE installer/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ${CMAKE_BINARY_DIR}/divasplit.suprx payload/divasplit.suprx
  FILE ${CMAKE_BINARY_DIR}/divasplit_qm.suprx payload/divasplit_qm.suprx
  FILE ${CMAKE_BINARY_DIR}/divasplit_prof.skprx payload/divasplit_prof.skprx
  FILE vitagrafix/divax.txt payload/vitagrafix/divax.txt
  FILE vitagrafix/divaf.txt payload/vitagrafix/divaf.txt
  FILE vitagrafix/divaf2nd.txt payload/vitagrafix/divaf2nd.txt
)
