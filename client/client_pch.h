#pragma once

#define _INCLUDE_TOKEN(p0, p1) <p0/p1>

#include <iostream>
#include <regex>
#include <functional>
#include <ostream>
#include <fstream>
#include <string>

#ifndef fan_verbose_print_level
  #define fan_verbose_print_level 1
#endif
#ifndef fan_debug
  #define fan_debug 0
#endif
#include <fan/types/types.h>

//#define loco_vulkan
//#define loco_compute_shader

#define loco_window
#define loco_context
//#define loco_legacy
#define loco_gl_major 3
#define loco_gl_minor 3

#define loco_rectangle
#define loco_light
#define loco_line
#define loco_circle
#define loco_button
#define loco_sprite
#define loco_dropdown
#define loco_pixel_format_renderer
#define loco_tp
#define loco_sprite_sheet

#define loco_grass_2d

#define loco_rectangle_3d
#define loco_model_3d
//
// 
#define loco_physics

#define loco_cuda
#define loco_nv12
#define loco_pixel_format_renderer

#include _FAN_PATH(graphics/loco.h)


#if defined(loco_imgui)
#include _FAN_PATH(graphics/gui/model_maker/maker.h)
#endif