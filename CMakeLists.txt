cmake_minimum_required(VERSION 3.15)
project(LangSwitch VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # статическая среда выполнения: exe запускается без доп. библиотек
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W3)
endif()

set(LS_SOURCES
  src/main.cpp
  src/layout_map.cpp
)

# Иконка приложения: если есть assets/app.ico, она встраивается в exe
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
  enable_language(RC)
  list(APPEND LS_SOURCES src/app.rc)
endif()

add_executable(LangSwitch WIN32 ${LS_SOURCES})

target_link_libraries(LangSwitch PRIVATE user32 winmm ole32 oleaut32 uuid advapi32)

if(MINGW)
  target_link_options(LangSwitch PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Модуль авто-переключения раскладки: отдельная библиотека autoswitch.dll, которая кладётся рядом с программой.
# Если файла src/autoswitch.cpp нет, библиотека не собирается, и авто-переключение недоступно.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/autoswitch.cpp")
  add_library(autoswitch SHARED src/autoswitch.cpp)
  set_target_properties(autoswitch PROPERTIES PREFIX "" OUTPUT_NAME "autoswitch")
  target_link_libraries(autoswitch PRIVATE user32)
  if(MINGW)
    target_link_options(autoswitch PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()
