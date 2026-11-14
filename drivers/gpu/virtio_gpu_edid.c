/*
 * VirtIO-GPU EDID parsing.
 *
 * EDID interpretation is transport-independent and host-testable. The mode
 * catalog decodes only timings actually declared by the connector: established
 * timings, standard timings and detailed timing descriptors. No NexsOS mode
 * policy or architecture-specific resolution list lives here.
 */
#include <drivers/gpu/gpu.h>
#include <drivers/virtio_gpu.h>

#define EDID_BASE_BYTES 128u
#define EDID_VERSION_OFFSET 18u
#define EDID_REVISION_OFFSET 19u
#define EDID_ESTABLISHED_OFFSET 35u
#define EDID_STANDARD_OFFSET 38u
#define EDID_STANDARD_COUNT 8u
#define EDID_FEATURE_OFFSET 24u
#define EDID_DTD_OFFSET 54u
#define EDID_DTD_BYTES 18u
#define EDID_DTD_COUNT 4u
#define EDID_FEATURE_PREFERRED_TIMING (1u << 1)

struct edid_established_timing {
  uint8_t byte;
  uint8_t mask;
  uint16_t width;
  uint16_t height;
  uint16_t refresh_hz;
  uint16_t flags;
};

static const struct edid_established_timing established_timings[] = {
    {35u, 0x80u, 720u, 400u, 70u, 0u},
    {35u, 0x40u, 720u, 400u, 88u, 0u},
    {35u, 0x20u, 640u, 480u, 60u, 0u},
    {35u, 0x10u, 640u, 480u, 67u, 0u},
    {35u, 0x08u, 640u, 480u, 72u, 0u},
    {35u, 0x04u, 640u, 480u, 75u, 0u},
    {35u, 0x02u, 800u, 600u, 56u, 0u},
    {35u, 0x01u, 800u, 600u, 60u, 0u},
    {36u, 0x80u, 800u, 600u, 72u, 0u},
    {36u, 0x40u, 800u, 600u, 75u, 0u},
    {36u, 0x20u, 832u, 624u, 75u, 0u},
    {36u, 0x10u, 1024u, 768u, 87u, GPU_DISPLAY_MODE_INTERLACED},
    {36u, 0x08u, 1024u, 768u, 60u, 0u},
    {36u, 0x04u, 1024u, 768u, 70u, 0u},
    {36u, 0x02u, 1024u, 768u, 75u, 0u},
    {36u, 0x01u, 1280u, 1024u, 75u, 0u},
    {37u, 0x80u, 1152u, 870u, 75u, 0u},
};

static int edid_base_valid(const uint8_t *edid, size_t size) {
  static const uint8_t header[8] = {
      0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x00u};
  if (edid == NULL || size < EDID_BASE_BYTES)
    return 0;
  for (size_t i = 0u; i < sizeof(header); ++i) {
    if (edid[i] != header[i])
      return 0;
  }
  uint8_t sum = 0u;
  for (size_t i = 0u; i < EDID_BASE_BYTES; ++i)
    sum = (uint8_t)(sum + edid[i]);
  return sum == 0u && edid[EDID_VERSION_OFFSET] == 1u;
}

static int mode_same(const struct gpu_display_mode *a,
                     const struct gpu_display_mode *b) {
  return a->width == b->width && a->height == b->height &&
         a->refresh_millihz == b->refresh_millihz;
}

static void mode_add(struct gpu_display_mode *modes, uint32_t capacity,
                     uint32_t *count, const struct gpu_display_mode *mode) {
  if (modes == NULL || count == NULL || mode == NULL || mode->width == 0u ||
      mode->height == 0u || mode->width > 8192u || mode->height > 8192u)
    return;
  for (uint32_t i = 0u; i < *count && i < capacity; ++i) {
    if (mode_same(&modes[i], mode)) {
      modes[i].flags |= mode->flags;
      return;
    }
  }
  if (*count < capacity) {
    modes[*count] = *mode;
    ++(*count);
  }
}

static void parse_established(const uint8_t *edid,
                              struct gpu_display_mode *modes,
                              uint32_t capacity, uint32_t *count) {
  (void)EDID_ESTABLISHED_OFFSET;
  for (size_t i = 0u;
       i < sizeof(established_timings) / sizeof(established_timings[0]); ++i) {
    const struct edid_established_timing *timing = &established_timings[i];
    if ((edid[timing->byte] & timing->mask) == 0u)
      continue;
    struct gpu_display_mode mode = {
        timing->width, timing->height,
        (uint32_t)timing->refresh_hz * 1000u, timing->flags};
    mode_add(modes, capacity, count, &mode);
  }
}

static uint32_t standard_height(uint32_t width, uint8_t aspect,
                                uint8_t revision) {
  if (aspect == 0u)
    return revision >= 3u ? width * 10u / 16u : width;
  if (aspect == 1u)
    return width * 3u / 4u;
  if (aspect == 2u)
    return width * 4u / 5u;
  return width * 9u / 16u;
}

static void parse_standard(const uint8_t *edid, struct gpu_display_mode *modes,
                           uint32_t capacity, uint32_t *count) {
  uint8_t revision = edid[EDID_REVISION_OFFSET];
  for (uint32_t i = 0u; i < EDID_STANDARD_COUNT; ++i) {
    size_t offset = EDID_STANDARD_OFFSET + (size_t)i * 2u;
    uint8_t first = edid[offset];
    uint8_t second = edid[offset + 1u];
    if ((first == 0x01u && second == 0x01u) ||
        (first == 0u && second == 0u))
      continue;
    uint32_t width = ((uint32_t)first + 31u) * 8u;
    uint32_t height = standard_height(width, second >> 6, revision);
    struct gpu_display_mode mode = {
        width, height, ((uint32_t)(second & 0x3fu) + 60u) * 1000u, 0u};
    mode_add(modes, capacity, count, &mode);
  }
}

static int parse_dtd(const uint8_t *dtd, struct gpu_display_mode *mode) {
  if (dtd == NULL || mode == NULL || (dtd[0] == 0u && dtd[1] == 0u))
    return -1;
  uint32_t width = (uint32_t)dtd[2] | ((uint32_t)(dtd[4] & 0xf0u) << 4);
  uint32_t hblank = (uint32_t)dtd[3] | ((uint32_t)(dtd[4] & 0x0fu) << 8);
  uint32_t height = (uint32_t)dtd[5] | ((uint32_t)(dtd[7] & 0xf0u) << 4);
  uint32_t vblank = (uint32_t)dtd[6] | ((uint32_t)(dtd[7] & 0x0fu) << 8);
  uint32_t htotal = width + hblank;
  uint32_t vtotal = height + vblank;
  uint32_t clock_10khz = (uint32_t)dtd[0] | ((uint32_t)dtd[1] << 8);
  if (width == 0u || height == 0u || width > 8192u || height > 8192u ||
      htotal == 0u || vtotal == 0u || clock_10khz == 0u)
    return -1;
  uint64_t refresh = (uint64_t)clock_10khz * UINT64_C(10000000);
  refresh /= (uint64_t)htotal * (uint64_t)vtotal;
  *mode = (struct gpu_display_mode){
      width, height, refresh > UINT32_MAX ? 0u : (uint32_t)refresh,
      (dtd[17] & 0x80u) != 0u ? GPU_DISPLAY_MODE_INTERLACED : 0u};
  return 0;
}

static void parse_detailed(const uint8_t *edid, struct gpu_display_mode *modes,
                           uint32_t capacity, uint32_t *count) {
  int preferred = (edid[EDID_FEATURE_OFFSET] &
                   EDID_FEATURE_PREFERRED_TIMING) != 0u;
  for (uint32_t i = 0u; i < EDID_DTD_COUNT; ++i) {
    const uint8_t *dtd = &edid[EDID_DTD_OFFSET + (size_t)i * EDID_DTD_BYTES];
    struct gpu_display_mode mode = {0};
    if (parse_dtd(dtd, &mode) != 0)
      continue;
    if (i == 0u && preferred)
      mode.flags |= GPU_DISPLAY_MODE_PREFERRED;
    mode_add(modes, capacity, count, &mode);
  }
}

int virtio_gpu_edid_modes(const uint8_t *edid, size_t size,
                          struct gpu_display_mode *modes, uint32_t capacity,
                          uint32_t *count) {
  if (modes == NULL || count == NULL || capacity == 0u ||
      capacity > GPU_DISPLAY_MODE_MAX || !edid_base_valid(edid, size))
    return -1;
  *count = 0u;
  parse_detailed(edid, modes, capacity, count);
  parse_standard(edid, modes, capacity, count);
  parse_established(edid, modes, capacity, count);
  return *count > 0u ? 0 : -1;
}

int virtio_gpu_edid_preferred_mode(const uint8_t *edid, size_t size,
                                   int *width, int *height) {
  struct gpu_display_mode modes[GPU_DISPLAY_MODE_MAX] = {{0}};
  uint32_t count = 0u;
  if (width == NULL || height == NULL ||
      virtio_gpu_edid_modes(edid, size, modes, GPU_DISPLAY_MODE_MAX, &count) !=
          0)
    return -1;
  for (uint32_t i = 0u; i < count; ++i) {
    if ((modes[i].flags & GPU_DISPLAY_MODE_PREFERRED) != 0u) {
      *width = (int)modes[i].width;
      *height = (int)modes[i].height;
      return 0;
    }
  }
  return -1;
}
