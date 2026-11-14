#include <drivers/virtio.h>

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
  if (test_avail_one_entry() != 0) return 1;
  if (test_avail_input_queue() != 0) return 2;
  if (test_used_one_entry() != 0) return 3;
  if (test_used_input_queue() != 0) return 4;
  return 0;
}
