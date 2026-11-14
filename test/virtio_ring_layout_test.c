#include <drivers/virtio.h>
#include <drivers/virtio_input.h>

static int test_input_descriptor_id_valid_boundary(void) {
  return virtio_input_descriptor_id_valid(VIRTIO_INPUT_QUEUE_SIZE - 1U) ? 0 : 1;
}

static int test_input_descriptor_id_rejects_device_overflow(void) {
  if (virtio_input_descriptor_id_valid(VIRTIO_INPUT_QUEUE_SIZE)) return 1;
  return virtio_input_descriptor_id_valid(UINT32_MAX) ? 2 : 0;
}

static int test_avail_one_entry(void) {
  return virtio_vring_avail_bytes(1U) == 8U ? 0 : 1;
}

static int test_avail_input_queue(void) {
  return virtio_vring_avail_bytes(16U) == 38U ? 0 : 1;
}

static int test_used_one_entry(void) {
  return virtio_vring_used_bytes(1U) == 14U ? 0 : 1;
}

static int test_used_input_queue(void) {
  size_t bytes = virtio_vring_used_bytes(16U);
  if (bytes != 134U) return 1;
  return bytes > 2U * 64U ? 0 : 2;
}

int main(void) {
  if (test_input_descriptor_id_valid_boundary() != 0) return 1;
  if (test_input_descriptor_id_rejects_device_overflow() != 0) return 2;
  if (test_avail_one_entry() != 0) return 3;
  if (test_avail_input_queue() != 0) return 4;
  if (test_used_one_entry() != 0) return 5;
  if (test_used_input_queue() != 0) return 6;
  return 0;
}
