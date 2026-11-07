# A project that uses an installed bbtree++, as a user's would: the install
# check (scripts/check-install.sh) builds it against a copy of the install
# moved elsewhere, so that no path of the source or build tree is baked in.
cmake_minimum_required(VERSION 3.28)
project(bbtree_installed_consumer LANGUAGES CXX)

find_package(bbtree 0.2 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE bbtree::bbtree bbtree::mmap bbtree::store)

enable_testing()
add_test(NAME consumer COMMAND consumer)
