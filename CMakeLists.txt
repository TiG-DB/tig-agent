cmake_minimum_required(VERSION 3.16)

project(
  tig-agent
  VERSION 0.2.0
  DESCRIPTION "Records a machine's timeline into a TiG repository"
  LANGUAGES C
)

option(TIG_AGENT_BUILD_TESTS "Build tests" ON)
set(TIG_AGENT_VERSION_SUFFIX "" CACHE STRING "Appended to the version, e.g. +42.1a2b3c4 for CI builds")
set(TIG_AGENT_VERSION "${PROJECT_VERSION}${TIG_AGENT_VERSION_SUFFIX}")

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# A pinned libgit2, built into libtig. The agent pushes with the git command, so libgit2 needs no network support
# and the binary depends on nothing beyond libc.
include(FetchContent)
FetchContent_Declare(
  libgit2
  URL https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.7.tar.gz
  URL_HASH SHA256=1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7
)
FetchContent_GetProperties(libgit2)
if(NOT libgit2_POPULATED)
  FetchContent_Populate(libgit2)
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(USE_HTTPS OFF CACHE STRING "" FORCE)
set(USE_SSH OFF CACHE STRING "" FORCE)
set(USE_SHA256 Builtin CACHE STRING "" FORCE)
set(USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(USE_ICONV OFF CACHE BOOL "" FORCE)
set(USE_BUNDLED_ZLIB ON CACHE STRING "" FORCE)
set(REGEX_BACKEND builtin CACHE STRING "" FORCE)

set(TIG_LIBGIT2_SOURCE_DIR "${libgit2_SOURCE_DIR}" CACHE PATH "" FORCE)
set(TIG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TIG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/tig-lib)

add_library(
  tig-agent-core STATIC
  src/actions.c
  src/config.c
  src/log.c
  src/process.c
  src/repository.c
  src/status.c
  src/sync.c
  src/telemetry.c
  src/daemon.c
  src/device.c
  src/docker.c
  src/inventory.c
  src/record.c
  src/sha256.c
)
target_include_directories(tig-agent-core PUBLIC src)
target_link_libraries(tig-agent-core PUBLIC tig::tig)
# events name the agent and its version as their source
target_compile_definitions(tig-agent-core PUBLIC TIG_AGENT_VERSION="${TIG_AGENT_VERSION}")

add_executable(tig-agent src/main.c)
target_link_libraries(tig-agent PRIVATE tig-agent-core)

foreach(_target tig-agent-core tig-agent)
  target_compile_features(${_target} PRIVATE c_std_99)
  target_compile_definitions(${_target} PRIVATE _DEFAULT_SOURCE)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${_target} PRIVATE -Wall -Wextra)
  endif()
endforeach()

if(TIG_AGENT_BUILD_TESTS)
  enable_testing()

  add_executable(tig-agent-config-test tests/config_test.c)
  target_link_libraries(tig-agent-config-test PRIVATE tig-agent-core)
  target_compile_definitions(tig-agent-config-test PRIVATE _DEFAULT_SOURCE)
  add_test(NAME tig-agent-config-test COMMAND tig-agent-config-test)

  add_executable(tig-agent-record-test tests/record_test.c)
  target_link_libraries(tig-agent-record-test PRIVATE tig-agent-core)
  target_compile_definitions(tig-agent-record-test PRIVATE _DEFAULT_SOURCE)
  add_test(NAME tig-agent-record-test COMMAND tig-agent-record-test)

  add_executable(tig-agent-docker-test tests/docker_test.c)
  target_link_libraries(tig-agent-docker-test PRIVATE tig-agent-core)
  target_compile_definitions(tig-agent-docker-test PRIVATE _DEFAULT_SOURCE)
  add_test(NAME tig-agent-docker-test COMMAND tig-agent-docker-test)

  add_test(
    NAME tig-agent-end-to-end-docker
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/end_to_end_docker.sh" "$<TARGET_FILE:tig-agent>"
  )
  set_tests_properties(tig-agent-end-to-end-docker PROPERTIES SKIP_RETURN_CODE 77)

  add_test(
    NAME tig-agent-end-to-end
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/end_to_end.sh" "$<TARGET_FILE:tig-agent>"
  )
endif()

# Packaging. Only the "agent" component is packaged, which leaves out libtig's and libgit2's install rules.
include(GNUInstallDirs)

install(TARGETS tig-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT agent)
install(FILES packaging/agent.conf DESTINATION /etc/tig-agent COMPONENT agent)
install(
  FILES
    packaging/systemd/tig-agentd.service
    packaging/systemd/tig-agent-update.service
    packaging/systemd/tig-agent-update.timer
  DESTINATION lib/systemd/system
  COMPONENT agent
)
install(PROGRAMS packaging/tig-agent-update DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/tig-agent COMPONENT agent)
install(FILES packaging/release-signers DESTINATION ${CMAKE_INSTALL_DATADIR}/tig-agent COMPONENT agent)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME tig-agent)
set(CPACK_PACKAGE_VERSION "${TIG_AGENT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT "jchown <jez.chown@gmail.com>")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_COMPONENTS_ALL agent)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser, curl, git, jq, openssh-client")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(
  CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/conffiles"
)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
