#include <kernel/driver.h>
#include <kernel/hal.h>

#include <stdint.h>

struct probe_state {
  int dma_calls;
  int fallback_calls;
  int violations;
  int dma_result;
  int fallback_result;
};

void hal_bus_scan(void) {}
void arch_virtio_scan(void) {}

int nxhal_pci_set_bus_master(uint32_t bdf, int enabled) {
  if (bdf == UINT32_C(0x00000300) && enabled != 0)
    return -1;
  if (bdf == UINT32_C(0x00000400) && enabled == 0)
    return -1;
  return 0;
}

static int probe_claim(struct hal_device *dev) {
  struct probe_state *state = (struct probe_state *)dev->priv;
  if (state == NULL || dev->driver == NULL) return -1;
  if ((dev->driver->flags & DEVICE_DRIVER_DMA) != 0U) {
    ++state->dma_calls;
    if (dev->dma_authorized != UINT8_C(1)) ++state->violations;
    return state->dma_result;
  }
  ++state->fallback_calls;
  if (dev->dma_authorized != UINT8_C(0)) ++state->violations;
  return state->fallback_result;
}

static struct hal_device make_device(uint16_t vendor, uint16_t device,
                                     uint32_t bdf,
                                     struct probe_state *state) {
  struct hal_device result = {0};
  result.bus_type = HAL_BUS_TYPE_PCI;
  result.vendor_id = vendor;
  result.device_id = device;
  result.pci_bdf = bdf;
  result.priv = state;
  return result;
}

static struct device_driver make_driver(uint16_t vendor, uint16_t device,
                                        uint32_t flags) {
  struct device_driver result = {
      .name = "test",
      .vendor = vendor,
      .device = device,
      .class_code = DRV_ANY_CLASS,
      .subclass = DRV_ANY_CLASS,
      .prog_if = DRV_ANY_CLASS,
      .flags = flags,
      .probe = probe_claim,
  };
  return result;
}

int main(void) {
  struct probe_state success = {0, 0, 0, 0, 0};
  struct probe_state grant_fail = {0, 0, 0, 0, 0};
  struct probe_state rollback = {0, 0, 0, -1, 0};
  struct probe_state quarantine = {0, 0, 0, -1, 0};
  struct device_driver success_dma = make_driver(0x1000, 1, DEVICE_DRIVER_DMA);
  struct device_driver grant_dma = make_driver(0x2000, 2, DEVICE_DRIVER_DMA);
  struct device_driver fallback = make_driver(0x3000, 3, 0U);
  struct device_driver rollback_dma = make_driver(0x3000, 3, DEVICE_DRIVER_DMA);
  struct device_driver quarantine_fallback = make_driver(0x4000, 4, 0U);
  struct device_driver quarantine_dma = make_driver(0x4000, 4, DEVICE_DRIVER_DMA);
  struct hal_device d1 = make_device(0x1000, 1, 0x100, &success);
  struct hal_device d2 = make_device(0x2000, 2, 0x300, &grant_fail);
  struct hal_device d3 = make_device(0x3000, 3, 0x301, &rollback);
  struct hal_device d4 = make_device(0x4000, 4, 0x400, &quarantine);
  struct hal_device *bound;

  driver_register(&success_dma);
  hal_register_device(&d1);
  driver_match_all();
  bound = hal_device_find(0x1000, 1, 0);
  if (bound == NULL || bound->driver != &success_dma ||
      bound->dma_authorized != UINT8_C(1) || success.dma_calls != 1 ||
      success.violations != 0) return 1;

  driver_register(&grant_dma);
  hal_register_device(&d2);
  driver_match_all();
  bound = hal_device_find(0x2000, 2, 0);
  if (bound == NULL || bound->driver != NULL ||
      bound->dma_authorized != UINT8_C(0) || grant_fail.dma_calls != 0)
    return 2;

  driver_register(&fallback);
  driver_register(&rollback_dma);
  hal_register_device(&d3);
  driver_match_all();
  bound = hal_device_find(0x3000, 3, 0);
  if (bound == NULL || bound->driver != &fallback ||
      bound->dma_authorized != UINT8_C(0) || rollback.dma_calls != 1 ||
      rollback.fallback_calls != 1 || rollback.violations != 0) return 3;

  driver_register(&quarantine_fallback);
  driver_register(&quarantine_dma);
  hal_register_device(&d4);
  driver_match_all();
  bound = hal_device_find(0x4000, 4, 0);
  if (bound == NULL || bound->driver != &quarantine_dma ||
      bound->dma_authorized != UINT8_C(1) || quarantine.dma_calls != 1 ||
      quarantine.fallback_calls != 0 || quarantine.violations != 0) return 4;
  return 0;
}
