/*
 * kernel/include/drivers/virtio_input.h
 * VirtIO Input Device Driver
 */
#ifndef _DRIVERS_VIRTIO_INPUT_H
#define _DRIVERS_VIRTIO_INPUT_H

#include <kernel/types.h>

#include <drivers/input_codes.h>

#define VIRTIO_INPUT_QUEUE_SIZE UINT32_C(16)

static inline int virtio_input_descriptor_id_valid(uint32_t id) {
  return id < VIRTIO_INPUT_QUEUE_SIZE;
}

/* Input Event Structure (8 bytes) */
struct virtio_input_event {
  uint16_t type;
  uint16_t code;
  int32_t value;
} __attribute__((packed));

/* Driver API */
void virtio_input_init(void);
int virtio_input_get_dev_count(void);

#endif /* _DRIVERS_VIRTIO_INPUT_H */
