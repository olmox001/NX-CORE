#include <drivers/gpu/gpu.h>
#include <drivers/virtio_gpu.h>

#define EDID_SIZE 128u

static void finalize_checksum(uint8_t *edid) {
  uint8_t sum = 0u;
  for (size_t i = 0u; i < EDID_SIZE - 1u; ++i)
    sum = (uint8_t)(sum + edid[i]);
  edid[EDID_SIZE - 1u] = (uint8_t)(0u - sum);
}

static void build_edid(uint8_t *edid) {
  static const uint8_t header[8] = {
      0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x00u};
  for (size_t i = 0u; i < EDID_SIZE; ++i)
    edid[i] = 0u;
  for (size_t i = 0u; i < sizeof(header); ++i)
    edid[i] = header[i];
  edid[18] = 1u;
  edid[19] = 4u;
  edid[24] = 1u << 1;

  /* Established 800x600 @ 60 Hz. */
  edid[35] = 0x01u;

  /* Standard 1440x900 @ 60 Hz, EDID 1.4 16:10 aspect. */
  for (size_t i = 0u; i < 8u; ++i) {
    edid[38u + i * 2u] = 0x01u;
    edid[39u + i * 2u] = 0x01u;
  }
  edid[38] = 0x95u;
  edid[39] = 0x00u;

  /* Preferred DTD: 1280x800, 400 horizontal blank, 31 vertical blank,
   * 83.50 MHz pixel clock (about 59.8 Hz). */
  edid[54] = 0x9eu;
  edid[55] = 0x20u;
  edid[56] = 0x00u;
  edid[57] = 0x90u;
  edid[58] = 0x51u;
  edid[59] = 0x20u;
  edid[60] = 0x1fu;
  edid[61] = 0x30u;
  finalize_checksum(edid);
}

static int test_valid_preferred_mode(void) {
  uint8_t edid[EDID_SIZE];
  int width = 0, height = 0;
  build_edid(edid);
  if (virtio_gpu_edid_preferred_mode(edid, sizeof(edid), &width, &height) != 0)
    return 1;
  return width == 1280 && height == 800 ? 0 : 2;
}

static int mode_present(const struct gpu_display_mode *modes, uint32_t count,
                        uint32_t width, uint32_t height, uint32_t refresh) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (modes[i].width == width && modes[i].height == height &&
        modes[i].refresh_millihz == refresh)
      return 1;
  }
  return 0;
}

static int test_declared_mode_catalog(void) {
  uint8_t edid[EDID_SIZE];
  struct gpu_display_mode modes[GPU_DISPLAY_MODE_MAX] = {{0}};
  uint32_t count = 0u;
  build_edid(edid);
  if (virtio_gpu_edid_modes(edid, sizeof(edid), modes,
                            GPU_DISPLAY_MODE_MAX, &count) != 0)
    return 1;
  if (count < 3u || modes[0].width != 1280u || modes[0].height != 800u ||
      !(modes[0].flags & GPU_DISPLAY_MODE_PREFERRED))
    return 2;
  if (modes[0].refresh_millihz < 59000u ||
      modes[0].refresh_millihz > 61000u)
    return 3;
  if (!mode_present(modes, count, 1440u, 900u, 60000u))
    return 4;
  return mode_present(modes, count, 800u, 600u, 60000u) ? 0 : 5;
}

static int test_invalid_checksum(void) {
  uint8_t edid[EDID_SIZE];
  struct gpu_display_mode modes[GPU_DISPLAY_MODE_MAX] = {{0}};
  uint32_t count = 0u;
  build_edid(edid);
  edid[10] ^= 1u;
  return virtio_gpu_edid_modes(edid, sizeof(edid), modes,
                               GPU_DISPLAY_MODE_MAX, &count) != 0
             ? 0
             : 1;
}

static int test_without_preferred_flag(void) {
  uint8_t edid[EDID_SIZE];
  int width = 0, height = 0;
  build_edid(edid);
  edid[24] = 0u;
  finalize_checksum(edid);
  return virtio_gpu_edid_preferred_mode(edid, sizeof(edid), &width, &height) != 0
             ? 0
             : 1;
}

static int test_short_block(void) {
  uint8_t edid[EDID_SIZE];
  struct gpu_display_mode modes[GPU_DISPLAY_MODE_MAX] = {{0}};
  uint32_t count = 0u;
  build_edid(edid);
  return virtio_gpu_edid_modes(edid, EDID_SIZE - 1u, modes,
                               GPU_DISPLAY_MODE_MAX, &count) != 0
             ? 0
             : 1;
}

static int test_bounded_capacity(void) {
  uint8_t edid[EDID_SIZE];
  struct gpu_display_mode mode[1] = {{0}};
  uint32_t count = 0u;
  build_edid(edid);
  if (virtio_gpu_edid_modes(edid, sizeof(edid), mode, 1u, &count) != 0)
    return 1;
  return count == 1u && mode[0].width == 1280u && mode[0].height == 800u ? 0
                                                                         : 2;
}

int main(void) {
  if (test_valid_preferred_mode() != 0) return 1;
  if (test_declared_mode_catalog() != 0) return 2;
  if (test_invalid_checksum() != 0) return 3;
  if (test_without_preferred_flag() != 0) return 4;
  if (test_short_block() != 0) return 5;
  if (test_bounded_capacity() != 0) return 6;
  return 0;
}
