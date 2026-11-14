#include <drivers/virtio.h>

#include <stdint.h>

struct dma_test_state {
  int calls;
  int fail_enable;
  int fail_disable;
  int last_enabled;
};

static int test_set_dma(struct virtio_device *dev, int enabled) {
  struct dma_test_state *state = (struct dma_test_state *)dev->priv;
  if (state == NULL) return -1;
  ++state->calls;
  state->last_enabled = enabled;
  if (enabled != 0 && state->fail_enable != 0) return -1;
  if (enabled == 0 && state->fail_disable != 0) return -1;
  return 0;
}

static const struct virtio_transport_ops test_ops = {
    .read32 = NULL,
    .write32 = NULL,
    .notify = NULL,
    .set_dma = test_set_dma,
};

static int test_acquire_success_and_duplicate(void) {
  struct dma_test_state state = {0};
  struct virtio_device dev = {.ops = &test_ops, .priv = &state};
  if (virtio_dma_acquire(&dev) != 0) return 1;
  if (dev.dma_authorized != UINT8_C(1) || state.calls != 1 ||
      state.last_enabled != 1) return 2;
  if (virtio_dma_acquire(&dev) == 0 || state.calls != 1) return 3;
  return 0;
}

static int test_acquire_failure_preserves_unowned(void) {
  struct dma_test_state state = {.fail_enable = 1};
  struct virtio_device dev = {.ops = &test_ops, .priv = &state};
  if (virtio_dma_acquire(&dev) == 0) return 1;
  if (dev.dma_authorized != UINT8_C(0) || state.calls != 1 ||
      state.last_enabled != 1) return 2;
  return 0;
}

static int test_release_success_and_inactive(void) {
  struct dma_test_state state = {0};
  struct virtio_device dev = {
      .ops = &test_ops, .dma_authorized = UINT8_C(1), .priv = &state};
  if (virtio_dma_release(&dev) != 0) return 1;
  if (dev.dma_authorized != UINT8_C(0) || state.calls != 1 ||
      state.last_enabled != 0) return 2;
  if (virtio_dma_release(&dev) != 0 || state.calls != 1) return 3;
  return 0;
}

static int test_release_failure_quarantines(void) {
  struct dma_test_state state = {.fail_disable = 1};
  struct virtio_device dev = {
      .ops = &test_ops, .dma_authorized = UINT8_C(1), .priv = &state};
  if (virtio_dma_release(&dev) == 0) return 1;
  if (dev.dma_authorized != UINT8_C(1) || state.calls != 1 ||
      state.last_enabled != 0) return 2;
  return 0;
}

int main(void) {
  if (test_acquire_success_and_duplicate() != 0) return 1;
  if (test_acquire_failure_preserves_unowned() != 0) return 2;
  if (test_release_success_and_inactive() != 0) return 3;
  if (test_release_failure_quarantines() != 0) return 4;
  return 0;
}
