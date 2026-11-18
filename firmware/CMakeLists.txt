cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(o2_thingy91x)

target_sources(app PRIVATE
  src/main.c
  src/gnss.c
  src/env.c
  src/manual_loc.c
)
target_sources_ifdef(CONFIG_APP_SENSOR_O2       app PRIVATE src/o2_sensor.c)
target_sources_ifdef(CONFIG_APP_SENSOR_IAQ_CORE app PRIVATE src/iaq_core.c)

# Embed the Let's Encrypt root CA (ISRG Root X1) as a C string
set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
zephyr_include_directories(${gen_dir})
generate_inc_file_for_target(app
  ${CMAKE_CURRENT_SOURCE_DIR}/cert/isrg_root_x1.pem
  ${gen_dir}/ca_cert.pem.inc)
