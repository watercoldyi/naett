cmake_minimum_required(VERSION 3.15)

# 供 CI 验证安装后的 naett package：只构建，不运行。
project(naett_consumer C)

find_package(naett CONFIG REQUIRED)

add_executable(naett_consumer main.c)
target_link_libraries(naett_consumer PRIVATE naett::naett)
