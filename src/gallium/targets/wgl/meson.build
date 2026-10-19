# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT

gallium_wgl_link_args = []

# The UWP GDI stubs at the end of gallium_wgl.def.in exist only in winsys/uwp, so a desktop build
# exports everything before them.
if with_uwp
  wgl_def_in = 'gallium_wgl.def.in'
else
  wgl_def_in = custom_target(
    'gallium_wgl_desktop.def.in',
    input : 'gallium_wgl.def.in',
    output : 'gallium_wgl_desktop.def.in',
    command : [prog_python, '-c',
               'import sys; open(sys.argv[2], "w").write(open(sys.argv[1]).read().split("; UWP GDI stub functions")[0])',
               '@INPUT@', '@OUTPUT@'],
  )
endif

wgl_def = custom_target(
  'gallium_wgl.def',
  input: wgl_def_in,
  output : 'gallium_wgl.def',
  command : gen_vs_module_defs_normal_command,
)

gallium_wgl_name = get_option('gallium-wgl-dll-name')
libgallium_wgl = shared_library(
  gallium_wgl_name,
  ['wgl.c','dummy_uwp.cpp'],
  vs_module_defs : wgl_def,
  include_directories : [
    inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux, inc_wgl, inc_gallium_winsys, inc_gallium_winsys_sw, inc_gallium_drivers,
  ],
  link_whole : [libwgl, libgdi_uwp],
  link_with : [
    libgallium, libglsl, libmesa, libwsgdi, libglapi_bridge, libglapi, libgalliumvl_stub, libgdi_uwp
  ],
  dependencies : [
    dep_ws2_32, idep_nir, idep_mesautil, driver_swrast,
    driver_d3d12, driver_zink, winsys_d3d12_wgl
  ],
  link_args : gallium_wgl_link_args,
  name_prefix : '',
  install : true,
)
libgallium_wgl_build_dir = meson.current_build_dir()

shared_glapi_lib = libgallium_wgl
