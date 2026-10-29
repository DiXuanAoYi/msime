cmake_minimum_required(VERSION 3.16)
project(fcitx5-msime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20) # fcitx5 的头文件用到 std::source_location
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 引擎本体是 Rust 的 msime-capi（cdylib）。先用 cargo 构建，再链接。
set(MSIME_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "engine-rs 目录")
set(MSIME_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target" CACHE PATH "cargo 产物目录")
set(MSIME_CAPI_LIB "${MSIME_CARGO_TARGET_DIR}/release/libmsime_capi.so")
add_custom_command(
  OUTPUT "${MSIME_CAPI_LIB}"
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${MSIME_CARGO_TARGET_DIR} cargo build --release -p msime-capi
  WORKING_DIRECTORY "${MSIME_ENGINE_DIR}"
  COMMENT "cargo build msime-capi"
  VERBATIM)
add_custom_target(msime_capi DEPENDS "${MSIME_CAPI_LIB}")

# 与 fcitx5 无关的核心：快照解析、按键映射、对 C 接口的封装。
add_library(msime_frontend STATIC src/snapshot.cpp src/key_map.cpp src/driver.cpp)
target_include_directories(msime_frontend PUBLIC src)
add_dependencies(msime_frontend msime_capi)
target_link_libraries(msime_frontend PUBLIC "${MSIME_CAPI_LIB}")

enable_testing()
add_executable(msime_frontend_test tests/frontend_test.cpp)
target_link_libraries(msime_frontend_test PRIVATE msime_frontend)
add_test(NAME frontend COMMAND msime_frontend_test)
set_tests_properties(frontend PROPERTIES SKIP_RETURN_CODE 77)

# fcitx5 插件；没装 fcitx5 开发包就只构建核心。
find_package(Fcitx5Core QUIET)
if(Fcitx5Core_FOUND)
  find_package(Fcitx5Utils REQUIRED)
  find_package(Fcitx5Config REQUIRED)
  add_library(msime MODULE src/engine.cpp)
  target_link_libraries(msime PRIVATE msime_frontend Fcitx5::Core Fcitx5::Config Fcitx5::Utils)
  install(TARGETS msime DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5")
  install(FILES "${MSIME_CAPI_LIB}" DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")
  install(FILES data/msime-addon.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon" RENAME msime.conf)
  install(FILES data/msime.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod")
  # 引擎集成测试：真实的 fcitx5 Instance + testfrontend 输入 nihao，核对上屏。
  find_package(Fcitx5ModuleTestFrontend QUIET)
  if(Fcitx5ModuleTestFrontend_FOUND)
    add_executable(msime_engine_test tests/engine_test.cpp)
    target_link_libraries(msime_engine_test PRIVATE Fcitx5::Core Fcitx5::Utils Fcitx5::Module::TestFrontend)
    add_dependencies(msime_engine_test msime)
    target_compile_definitions(msime_engine_test PRIVATE FCITX_MSIME_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}" FCITX_SYSTEM_ADDON_DIR="${FCITX_INSTALL_LIBDIR}/fcitx5")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon" "${CMAKE_CURRENT_BINARY_DIR}/testdata/inputmethod")
    configure_file(data/msime-addon.conf "${CMAKE_CURRENT_BINARY_DIR}/testdata/addon/msime.conf" COPYONLY)
    configure_file(data/msime.conf "${CMAKE_CURRENT_BINARY_DIR}/testdata/inputmethod/msime.conf" COPYONLY)
    add_test(NAME engine COMMAND msime_engine_test)
    set_tests_properties(engine PROPERTIES SKIP_RETURN_CODE 77)
  endif()
else()
  message(STATUS "没找到 Fcitx5Core：只构建核心和测试，不构建插件")
endif()
