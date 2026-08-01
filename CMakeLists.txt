cmake_minimum_required(VERSION 3.16)

project(rtt_opcua VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OROCOS-RTT REQUIRED)
find_package(open62541 CONFIG REQUIRED)
find_package(open62541pp 0.21.1 CONFIG REQUIRED)

include(${OROCOS-RTT_USE_FILE})

option(RTT_OPCUA_WARNINGS_AS_ERRORS "Treat maintained-source warnings as errors" OFF)
option(BUILD_TESTING "Build rtt_opcua tests" OFF)

set(OROCOS_NO_AUTO_LINKING TRUE)

orocos_library(
  orocos-rtt-opcua
  src/node_id.cpp
  src/client_session.cpp
  src/object_model.cpp
  src/operation_dispatcher.cpp
  src/port_bridge.cpp
  src/remote_operation.cpp
  src/remote_port.cpp
  src/server.cpp
  src/server_options.cpp
  src/task_context_proxy.cpp
  src/type_descriptor.cpp
  src/type_protocol.cpp
)

target_compile_features(orocos-rtt-opcua PUBLIC cxx_std_20)
target_include_directories(
  orocos-rtt-opcua
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${OROCOS-RTT_INCLUDE_DIRS}
)
target_link_libraries(
  orocos-rtt-opcua
  ${OROCOS-RTT_LIBRARIES}
  open62541pp::open62541pp
  open62541::open62541
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(orocos-rtt-opcua PRIVATE -Wall -Wextra -Wpedantic)
  if(RTT_OPCUA_WARNINGS_AS_ERRORS)
    target_compile_options(orocos-rtt-opcua PRIVATE -Werror)
  endif()
endif()

orocos_plugin(
  rtt-transport-opcua
  src/type_transport_plugin.cpp
)
target_compile_features(rtt-transport-opcua PRIVATE cxx_std_20)
target_include_directories(rtt-transport-opcua PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rtt-transport-opcua orocos-rtt-opcua)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rtt-transport-opcua PRIVATE -Wall -Wextra -Wpedantic)
  if(RTT_OPCUA_WARNINGS_AS_ERRORS)
    target_compile_options(rtt-transport-opcua PRIVATE -Werror)
  endif()
endif()

install(DIRECTORY include/rtt DESTINATION include/orocos)
install(FILES package.xml DESTINATION share/rtt_opcua)

if(BUILD_TESTING)
  enable_testing()

  add_executable(
    rtt_opcua_foundation_test
    tests/foundation_test.cpp
  )
  target_link_libraries(rtt_opcua_foundation_test PRIVATE orocos-rtt-opcua)
  target_compile_features(rtt_opcua_foundation_test PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtt_opcua_foundation_test PRIVATE -Wall -Wextra -Wpedantic)
    if(RTT_OPCUA_WARNINGS_AS_ERRORS)
      target_compile_options(rtt_opcua_foundation_test PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME rtt_opcua_foundation_test COMMAND rtt_opcua_foundation_test)

  add_executable(rtt_opcua_server_test tests/server_test.cpp)
  target_link_libraries(rtt_opcua_server_test PRIVATE orocos-rtt-opcua)
  target_compile_features(rtt_opcua_server_test PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtt_opcua_server_test PRIVATE -Wall -Wextra -Wpedantic)
    if(RTT_OPCUA_WARNINGS_AS_ERRORS)
      target_compile_options(rtt_opcua_server_test PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME rtt_opcua_server_test COMMAND rtt_opcua_server_test)

  add_executable(rtt_opcua_type_protocol_test tests/type_protocol_test.cpp)
  target_link_libraries(
    rtt_opcua_type_protocol_test
    orocos-rtt-opcua
    rtt-transport-opcua
    ${OROCOS-RTT_TYPEKIT_LIBRARIES}
  )
  target_compile_features(rtt_opcua_type_protocol_test PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtt_opcua_type_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
    if(RTT_OPCUA_WARNINGS_AS_ERRORS)
      target_compile_options(rtt_opcua_type_protocol_test PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME rtt_opcua_type_protocol_test COMMAND rtt_opcua_type_protocol_test)

  add_executable(rtt_opcua_object_model_test tests/object_model_test.cpp)
  target_link_libraries(
    rtt_opcua_object_model_test
    PRIVATE
      orocos-rtt-opcua
      ${OROCOS-RTT_TYPEKIT_LIBRARIES}
  )
  target_compile_features(rtt_opcua_object_model_test PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtt_opcua_object_model_test PRIVATE -Wall -Wextra -Wpedantic)
    if(RTT_OPCUA_WARNINGS_AS_ERRORS)
      target_compile_options(rtt_opcua_object_model_test PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME rtt_opcua_object_model_test COMMAND rtt_opcua_object_model_test)

  add_executable(rtt_opcua_task_context_proxy_test tests/task_context_proxy_test.cpp)
  target_link_libraries(
    rtt_opcua_task_context_proxy_test
    PRIVATE
      orocos-rtt-opcua
      ${OROCOS-RTT_TYPEKIT_LIBRARIES}
  )
  target_compile_features(rtt_opcua_task_context_proxy_test PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtt_opcua_task_context_proxy_test PRIVATE -Wall -Wextra -Wpedantic)
    if(RTT_OPCUA_WARNINGS_AS_ERRORS)
      target_compile_options(rtt_opcua_task_context_proxy_test PRIVATE -Werror)
    endif()
  endif()
  add_test(
    NAME rtt_opcua_task_context_proxy_test
    COMMAND rtt_opcua_task_context_proxy_test
  )
endif()

orocos_generate_package(VERSION ${PROJECT_VERSION} DEPENDS open62541)
