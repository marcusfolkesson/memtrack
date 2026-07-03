cmake_minimum_required(VERSION 3.16)
project(memtrack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# memtrack library
add_library(memtrack SHARED memtrack.cpp)
set_target_properties(memtrack PROPERTIES
    OUTPUT_NAME "memtrack"
    PREFIX ""           # produce memtrack.so, not libmemtrack.so
)
target_compile_options(memtrack PRIVATE -Wall -Wextra)
target_link_libraries(memtrack PRIVATE dl pthread)

# Test application
add_executable(test_app test_app.cpp)
target_compile_options(test_app PRIVATE -O0 -g -Wall -Wextra)
target_link_options(test_app PRIVATE -rdynamic)
target_link_libraries(test_app PRIVATE pthread)

#memview ───────────────────────────────────────────────────────────────────
# Try wide-char ncurses first fall back to narrow.
set(CURSES_NEED_WIDE TRUE)
find_package(Curses QUIET)
if(NOT CURSES_FOUND)
    set(CURSES_NEED_WIDE FALSE)
    find_package(Curses REQUIRED)
endif()

include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES ${CURSES_LIBRARIES})
check_symbol_exists(setcchar "ncurses.h" MEMVIEW_HAVE_NCURSESW)
unset(CMAKE_REQUIRED_LIBRARIES)

add_executable(memview memview.cpp)
target_compile_options(memview PRIVATE -Wall -Wextra)
target_compile_definitions(memview PRIVATE HAVE_NCURSESW=$<BOOL:${MEMVIEW_HAVE_NCURSESW}>)
target_include_directories(memview PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(memview PRIVATE ${CURSES_LIBRARIES})

if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    add_test(NAME run_test_app
        COMMAND ${CMAKE_COMMAND}
            -DTEST_APP=$<TARGET_FILE:test_app>
            -DMEMTRACK_SO=$<TARGET_FILE:memtrack>
            -DLOG_FILE=${CMAKE_CURRENT_BINARY_DIR}/mt.log
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_test_app.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    add_test(NAME verify_log
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/verify.sh
                ${CMAKE_CURRENT_BINARY_DIR}/mt.log
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(verify_log PROPERTIES DEPENDS run_test_app)
endif()
