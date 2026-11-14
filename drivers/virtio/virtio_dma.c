#include <drivers/virtio.h>

int virtio_dma_acquire(virtio_handle_t dev) {
  if (dev == NULL || dev->ops == NULL || dev->ops->set_dma == NULL)
    return -1;
  if (dev->dma_authorized != UINT8_C(0))
    return -1;
  if (dev->ops->set_dma(dev, 1) != 0)
    return -1;
  dev->dma_authorized = UINT8_C(1);
  return 0;
}

int virtio_dma_release(virtio_handle_t dev) {
  if (dev == NULL || dev->ops == NULL || dev->ops->set_dma == NULL)
    return -1;
  if (dev->dma_authorized == UINT8_C(0))
    return 0;
  if (dev->ops->set_dma(dev, 0) != 0)
    return -1;
  dev->dma_authorized = UINT8_C(0);
  return 0;
}
