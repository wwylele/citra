set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

add_executable(svc-wrap-test
    wrap.cpp
)

create_target_directory_groups(svc-wrap-test)

target_link_libraries(svc-wrap-test PRIVATE common core)
target_link_libraries(svc-wrap-test PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)
