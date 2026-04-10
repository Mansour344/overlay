cmake_minimum_required(VERSION 3.10)
project(browser_overlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Some Ubuntu systems split pkg-config files across two dirs
set(ENV{PKG_CONFIG_PATH}
    "/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(CAIRO      REQUIRED cairo)
pkg_check_modules(CAIRO_XLIB REQUIRED cairo-xlib)
pkg_check_modules(LIBCURL    REQUIRED libcurl)
pkg_check_modules(XCOMPOSITE REQUIRED xcomposite)
pkg_check_modules(XRENDER    REQUIRED xrender)
pkg_check_modules(X11        REQUIRED x11)
pkg_check_modules(LIBPNG     REQUIRED libpng)

add_executable(overlay overlay.cpp)

target_include_directories(overlay SYSTEM PRIVATE
    ${CAIRO_INCLUDE_DIRS}
    ${CAIRO_XLIB_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${LIBPNG_INCLUDE_DIRS}
    /usr/include/X11
)

target_link_libraries(overlay PRIVATE
    ${CAIRO_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${XCOMPOSITE_LIBRARIES}
    ${XRENDER_LIBRARIES}
    ${X11_LIBRARIES}
    ${LIBPNG_LIBRARIES}
    Threads::Threads
)

target_compile_options(overlay PRIVATE
    -Wall -Wextra -O2
    ${CAIRO_CFLAGS_OTHER}
)
