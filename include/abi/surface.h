/* Generated from kernel/contracts/surface-v1.json. Do not edit. */
#ifndef NX_SURFACE_ABI_H
#define NX_SURFACE_ABI_H
#include <stdint.h>
#include <stddef.h>
#include <os1_input.h>
#define OS1_SURFACE_ABI_VERSION 1U
#define OS1_SURFACE_TIMING_COMPLETION 1U
#define OS1_RESOURCE_SURFACE_EVENTS "surface/events"
#define OS1_RESOURCE_DISPLAY "display/0"
#define OS1_RESOURCE_KEYBOARD "input/keyboard"
#define OS1_RESOURCE_POINTER "input/pointer"
#define OS1_RESOURCE_WINDOW_STORAGE "surface/window-v1"
#define OS1_RESOURCE_BUFFER "memory/buffer"
#define OS1_BUFFER_INFO 0x00080001U
#define OS1_BUFFER_ALLOCATE 0x00080002U
#define OS1_BUFFER_MAP 0x00080003U
#define OS1_BUFFER_UNMAP 0x00080004U
#define OS1_BUFFER_WRITABLE 1U
#define OS1_BUFFER_ACCOUNT_UNKNOWN UINT64_MAX
#define OS1_RESOURCE_DISPLAY_MODES "display/0/modes"
#define OS1_DISPLAY_MODE_QUERY 0U
#define OS1_DISPLAY_MODE_SET 1U
#define OS1_DISPLAY_MODE_CURRENT 1U
#define OS1_DISPLAY_MODE_PREFERRED 2U
#define OS1_DISPLAY_MODE_INTERLACED 4U
#define OS1_DISPLAY_MODE_MAX 32U
#define OS1_RESOURCE_DISPLAY_WORKSPACE "display/0/workspace"
#define OS1_DISPLAY_WORKSPACE_AUTO 0U
#define OS1_DISPLAY_WORKSPACE_MANUAL 1U
#define OS1_RESOURCE_SURFACE_LAYOUT "surface/layout-v1"
#define OS1_SURFACE_LAYOUT_ANCHOR_NONE 0U
#define OS1_SURFACE_LAYOUT_ANCHOR_TOP 1U
#define OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM 2U
#define OS1_SURFACE_LAYOUT_LAYER_NORMAL 0U
#define OS1_SURFACE_LAYOUT_LAYER_OVERLAY 1U
struct os1_surface_state_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t generation;
  uint64_t monotonic_ns;
  uint32_t width;
  uint32_t height;
  uint32_t timing;
  uint32_t reserved;
};
struct os1_input_request_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t mode;
  uint32_t reserved;
  struct os1_raw_input_event_v1 event;
};
struct os1_surface_move_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int32_t id;
  int32_t x;
  int32_t y;
};
struct os1_buffer_request_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t operation;
  uint32_t flags;
  uint64_t byte_length;
  uint64_t page_offset;
  uint64_t address;
  uint64_t completed;
  uint64_t total_pages;
  uint64_t work_units;
  uint64_t hardware_cycles;
};
struct os1_display_mode_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t operation;
  uint32_t index;
  uint32_t count;
  uint32_t reserved;
  uint64_t generation;
  uint32_t width;
  uint32_t height;
  uint32_t refresh_millihz;
  uint32_t flags;
};
struct os1_display_workspace_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t mode;
  uint32_t zoom_percent;
  uint32_t base_width;
  uint32_t base_height;
  uint32_t physical_width;
  uint32_t physical_height;
  uint32_t logical_width;
  uint32_t logical_height;
};
struct os1_surface_layout_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int32_t window_id;
  uint32_t anchor;
  uint32_t layer;
  uint32_t order;
  uint32_t margin;
  uint32_t reserved;
  uint32_t width;
  uint32_t height;
  int32_t x;
  int32_t y;
  int32_t work_x;
  int32_t work_y;
  uint32_t work_width;
  uint32_t work_height;
};
#endif
