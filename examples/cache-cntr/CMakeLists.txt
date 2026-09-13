set(eapp_bin  cache-cntr)
set(eapp_src  eapp/eapp.c)
set(host_bin  cache-cntr-runner)
set(host_src  host/host.cpp)
set(package_name   "cache-cntr.ke")
set(package_script "./cache-cntr-runner cache-cntr eyrie-rt loader.bin")

set(eyrie_plugins "none")
# eapp: bare, no libc. Uses the SDK ocall() interface only.
add_executable(${eapp_bin} ${eapp_src})
target_link_libraries(${eapp_bin} "-nostdlib -static"
  ${KEYSTONE_LIB_EAPP} ${KEYSTONE_LIB_EDGE})
target_include_directories(${eapp_bin}
  PUBLIC ${KEYSTONE_SDK_DIR}/include/app
  PUBLIC ${KEYSTONE_SDK_DIR}/include/edge)

# tunables: cmake -DRDCYCLE_ITERS=<n>
if(DEFINED RDCYCLE_ITERS)
  target_compile_definitions(${eapp_bin} PRIVATE ITERS=${RDCYCLE_ITERS}UL)
endif()

# host: no verifier.
add_executable(${host_bin} ${host_src})
target_link_libraries(${host_bin} ${KEYSTONE_LIB_HOST} ${KEYSTONE_LIB_EDGE})
set_target_properties(${host_bin}
  PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(${host_bin}
  PUBLIC ${KEYSTONE_SDK_DIR}/include/common
  PUBLIC ${KEYSTONE_SDK_DIR}/include/host
  PUBLIC ${KEYSTONE_SDK_DIR}/include/edge)

# Eyrie runtime
set(eyrie_files_to_copy .options_log eyrie-rt loader.bin)
add_eyrie_runtime(${eapp_bin}-eyrie
  ${eyrie_plugins}
  ${eyrie_files_to_copy})

# package
add_keystone_package(${eapp_bin}-package
  ${package_name}
  ${package_script}
  ${eyrie_files_to_copy} ${eapp_bin} ${host_bin})

add_dependencies(${eapp_bin}-package ${eapp_bin}-eyrie)
add_dependencies(examples ${eapp_bin}-package)
