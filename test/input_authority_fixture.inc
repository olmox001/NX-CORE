/* Exercise the actual kernel/resources/input.c compiled verbatim.
 * Mocks represent HAL and process authority, not an alternate input path. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <caps.h>
#include <object.h>
#include <surface.h>
struct process { uint32_t caps; int pid, parent_pid, owner_pid; };
struct nx_resource_ops {
  long (*read)(void *, void *, size_t);
  long (*write)(void *, const void *, size_t);
  long (*wait)(void *, long);
  void (*release)(void *);
};
struct kobject {
  const struct nx_resource_ops *resource_ops;
  void *resource_state;
};
static int injected_count = 0;
static unsigned observed_role, observed_mode;
static int proc_has_cap(const struct process *p, uint32_t bit) {
  return p != NULL && (p->caps & bit) != 0U;
}
static int hal_copy_from_user(void *out, const void *in, size_t n) {
  if (out == NULL || in == NULL) return -1;
  memcpy(out, in, n);
  return 0;
}
static int hal_copy_to_user(void *out, const void *in, size_t n) {
  if (out == NULL || in == NULL) return -1;
  memcpy(out, in, n);
  return 0;
}
static long OS1libkernel_input_read_raw(struct os1_raw_input_event_v1 *event,
                                        unsigned role, unsigned mode) {
  ++injected_count;
  observed_role = role;
  observed_mode = mode;
  event->type = 1U;
  event->code = 30U;
  event->value = 1;
  return 1L;
}
#include "real_input_access.inc"

static int test_router_acquires_without_window(void) {
  struct process keyboard_service = {.caps = CAP_KEYBOARD_RAW | CAP_IPC_ANY};
  struct kobject resource = {0};
  if (nx_input_resource_acquire(&keyboard_service, OS1_RESOURCE_POINTER,
      OS1_RIGHT_READ, &resource) != -EPERM) return 3;
  int acquired = nx_input_resource_acquire(
      &keyboard_service, OS1_RESOURCE_KEYBOARD, OS1_RIGHT_READ, &resource);
  if (acquired != 0 || resource.resource_ops == NULL ||
      (uintptr_t)resource.resource_state != OS1_INPUT_ROLE_KEYBOARD)
    return 1;
  struct os1_input_request_v1 req = {
      .abi_version = OS1_SURFACE_ABI_VERSION,
      .struct_size = sizeof(req),
      .mode = OS1_INPUT_READ_WAIT
  };
  long read = resource.resource_ops->read(resource.resource_state,
                                          &req, sizeof(req));
  if (read != 1L || injected_count != 1 ||
      observed_role != OS1_INPUT_ROLE_KEYBOARD ||
      observed_mode != OS1_INPUT_READ_WAIT ||
      req.event.code != 30U || req.event.value != 1)
    return 2;
  return 0;
}

static int test_pointer_acquires_without_window(void) {
  struct process mouse_service = {.caps = CAP_POINTER_RAW};
  struct kobject resource = {0};
  if (nx_input_resource_acquire(&mouse_service, OS1_RESOURCE_KEYBOARD,
       OS1_RIGHT_READ, &resource) != -EPERM) return 3;
  if (nx_input_resource_acquire(&mouse_service, OS1_RESOURCE_POINTER,
       OS1_RIGHT_READ, &resource) != 0 ||
      (uintptr_t)resource.resource_state != OS1_INPUT_ROLE_MOUSE)
    return 1;
  struct os1_input_request_v1 req = {
      .abi_version = OS1_SURFACE_ABI_VERSION,
      .struct_size = sizeof(req), .mode = OS1_INPUT_READ_TRY
  };
  if (resource.resource_ops->read(resource.resource_state, &req,
      sizeof(req)) != 1L || observed_role != OS1_INPUT_ROLE_MOUSE ||
      observed_mode != OS1_INPUT_READ_TRY) return 2;
  return 0;
}

static int test_apps_cannot_acquire_raw(void) {
  struct process app = {.caps = CAP_WINDOW | CAP_IPC_ANY};
  struct kobject resource = {0};
  if (nx_input_resource_acquire(&app, OS1_RESOURCE_KEYBOARD,
        OS1_RIGHT_READ, &resource) != -EPERM) return 1;
  if (nx_input_resource_acquire(&app, OS1_RESOURCE_POINTER,
        OS1_RIGHT_READ, &resource) != -EPERM) return 2;
  if (nx_input_resource_acquire(NULL, OS1_RESOURCE_KEYBOARD,
        OS1_RIGHT_READ, &resource) != -EPERM) return 3;
  if (nx_input_resource_acquire(&app, "input/typo",
        OS1_RIGHT_READ, &resource) != -ENOENT) return 4;
  if (resource.resource_ops != NULL) return 5;
  return 0;
}

static int test_invalid_provider_requests(void) {
  struct process router = {.caps = CAP_KEYBOARD_RAW};
  struct kobject resource = {0};
  if (nx_input_resource_acquire(&router, OS1_RESOURCE_KEYBOARD,
        OS1_RIGHT_WRITE, &resource) != -EPERM) return 1;
  if (nx_input_resource_acquire(&router, OS1_RESOURCE_KEYBOARD,
        OS1_RIGHT_READ | OS1_RIGHT_DESTROY, &resource) != -EPERM) return 2;
  if (nx_input_resource_acquire(&router, OS1_RESOURCE_KEYBOARD,
        OS1_RIGHT_READ, &resource) != 0) return 3;
  struct os1_input_request_v1 req = {
      .abi_version = 99U, .struct_size = sizeof(req)
  };
  if (resource.resource_ops->read(resource.resource_state,
      &req, sizeof(req)) != -EINVAL) return 4;
  req.abi_version = OS1_SURFACE_ABI_VERSION;
  req.mode = OS1_INPUT_READ_WAIT + 1U;
  if (resource.resource_ops->read(resource.resource_state,
      &req, sizeof(req)) != -EINVAL) return 5;
  if (resource.resource_ops->read(resource.resource_state,
      &req, sizeof(req)-1U) != -EINVAL) return 6;
  return 0;
}

int main(void) {
  struct process client = {.pid = 41, .caps = CAP_IPC_ANY};
  struct process keyboard = {.pid = 7, .caps = CAP_KEYBOARD_RAW};
  struct process pointer = {.pid = 8, .caps = CAP_POINTER_RAW};
  struct process target = {.pid = 90, .parent_pid = 12, .owner_pid = 22};
  if (ipc_input_allowed(&client, &target, IPC_TYPE_INPUT) ||
      ipc_input_allowed(&client, &target, IPC_TYPE_MOUSE) ||
      ipc_input_allowed(&pointer, &target, IPC_TYPE_INPUT) ||
      ipc_input_allowed(&keyboard, &target, IPC_TYPE_MOUSE)) return 41;
  if (!ipc_input_allowed(&keyboard, &target, IPC_TYPE_INPUT) ||
      !ipc_input_allowed(&pointer, &target, IPC_TYPE_MOUSE) ||
      !ipc_input_allowed(&client, &target, IPC_TYPE_NOTIFY)) return 42;
  target.parent_pid = client.pid;
  if (!ipc_input_allowed(&client, &target, IPC_TYPE_INPUT)) return 43;
  target.parent_pid = 12;
  target.owner_pid = client.pid;
  if (!ipc_input_allowed(&client, &target, IPC_TYPE_INPUT)) return 44;
  int a = test_router_acquires_without_window();
  if (a != 0) return a;
  int b = test_pointer_acquires_without_window();
  if (b != 0) return 10+b;
  int c = test_apps_cannot_acquire_raw();
  if (c != 0) return 20+c;
  int d = test_invalid_provider_requests();
  if (d != 0) return 30+d;
  return 0;
}
