project(
  'libfprint-2-tod1-r503-arduino',
  ['c'],
  version: '1.1.0',
  license: 'MIT',
  default_options: [
    'buildtype=debugoptimized',
    'warning_level=1',
    'c_std=c11',
  ],
  meson_version: '>= 1.0.0',
)

add_project_arguments(['-D_GNU_SOURCE'], language: 'c')

libfprint_tod_dep = dependency('libfprint-2-tod-1')
systemd_dep = dependency('systemd')
libsystemd_dep = dependency('libsystemd')

shared_module(
  'fprint-2-tod1-r503-arduino',
  sources: [
    'tod-r503-arduino/tod-r503-arduino.c',
    'tod-r503-arduino/tod-r503-arduino.h',
  ],
  dependencies: libfprint_tod_dep,
  install: true,
  install_dir: libfprint_tod_dep.get_variable(pkgconfig: 'tod_driversdir'),
)

executable(
  'r503-arduinod',
  'r503-arduinod/r503-arduinod.c',
  dependencies: libsystemd_dep,
  c_args: [
    '-DR503_ARDUINOD_SERIAL="/dev/ttyUSB0"',
    '-DR503_ARDUINOD_SOCKET="/run/r503-arduinod.sock"',
  ],
  install: true,
  install_dir: get_option('bindir'),
)

install_data(
  'r503-arduinod/r503-arduinod.service',
  install_dir: systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir'),
  install_mode: 'rw-r--r--',
)

install_data(
  'fprintd.service.d/r503-arduino.conf',
  install_dir: systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir') / 'fprintd.service.d',
  install_mode: 'rw-r--r--',
)
