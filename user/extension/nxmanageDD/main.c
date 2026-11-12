/*
 * nxmanageDD — first NX-CORE user extension, executed in user mode.
 *
 * Only the bootstrapped PID1 receives machine authority. No process paths,
 * startup order, service names or restart policy live in Core's main().
 * This generic manager reads its initializer from a protected VFS file,
 * creates ROOT service children, and owns their real process lifecycles.
 * nxinit supplies the declarative catalog over a kernel-stamped PORT queue.
 */
#include <nxmanage.h>
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

struct nxm_slot {
  char name[NXMANAGE_NAME_MAX]; /* authoritative registry object identity */
  int named;
  char path[56];
  char args[48];
  char registry[40];
  int pid;
  uint32_t mode;
  uint32_t caps; /* manifest-granted capability mask */
  uint32_t auth_set;
  uint32_t generation;
  uint32_t backoff_ms;
  uint64_t started_ms;
  uint64_t due_ms;
  int active;
  int once_finished;
  int queued;
  int last_result; /* real PID for successful completed one-shot, else errno */
  int ack_target;  /* pending one-shot completion reply, kernel-stamped PID */
  uint64_t deadline_ms;
};

struct nxm_library {
  char name[NXMANAGE_LIBRARY_NAME_MAX];
  char path[NXMANAGE_LIBRARY_PATH_MAX];
  char abi[NXMANAGE_LIBRARY_ABI_MAX];
  int named;
  int active;
};

/* Owned exclusively by the single-threaded PID1 manager event loop.
 * State cannot be accessed or reentered by IPC handlers on other CPUs. */
struct nxm_state {
  struct nxm_slot slots[NXMANAGE_SLOTS];
  struct nxm_library libraries[NXMANAGE_LIBRARY_SLOTS];
  char init_path[96];
  char project[NXMANAGE_PROJECT_MAX];
  int init_pid;
  int init_started;
  int init_finished;
};

static int nxm_string(const char *src, unsigned cap, unsigned *len) {
  if (src == NULL || len == NULL)
    return -1;
  for (unsigned i = 0U; i < 64U; ++i) {
    if (i >= cap)
      return -1;
    if (src[i] == '\0') {
      *len = i;
      return 0;
    }
  }
  return -1;
}

static int nxm_boot_init_path(char init_path[96]) {
  char line[102U];
  int n = file_read(NXMANAGE_INIT_CONFIG, line, sizeof(line) - 1U, 0);
  if (n < 12 || n >= (int)sizeof(line))
    return -1;
  line[n] = '\0';
  if (n > 0 && line[n - 1] == '\n')
    line[--n] = '\0';
  if (n > 0 && line[n - 1] == '\r')
    line[--n] = '\0';
  if (strncmp(line, "init=/sbin/", 11U) != 0 || n <= 11)
    return -1;
  for (int i = 11; i < 102; ++i) {
    if (i >= n)
      break;
    char ch = line[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return -1;
  }
  if (n - 4 > 96)
    return -1;
  for (int k = 0; k < 96; ++k) {
    if (k > n - 5)
      break;
    init_path[k] = line[k + 5];
  }
  return 0;
}

static int nxm_boot_project(char project[NXMANAGE_PROJECT_MAX]) {
  char line[NXMANAGE_PROJECT_MAX + 2U] = {0};
  int n = file_read(NXMANAGE_PROJECT_CONFIG, line, sizeof(line) - 1U, 0);

  if (n <= 0 || n >= (int)sizeof(line))
    return -1;
  if (n > 0 && line[n - 1] == '\n')
    --n;
  if (n > 0 && line[n - 1] == '\r')
    --n;
  if (n <= 0 || n >= (int)NXMANAGE_PROJECT_MAX)
    return -1;
  for (unsigned i = 0U; i + 1U < NXMANAGE_PROJECT_MAX; ++i) {
    if (i < (unsigned)n) {
      char ch = line[i];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
        return -1;
      project[i] = ch;
    }
  }
  project[n] = 0;
  return 0;
}

static void nxm_log_manager(const struct nxm_state *state) {
  int written = printf("[NXMANAGE] System=%s init=%s manager-ready pid=1\n",
                       state->project, state->init_path);
  if (written < 0)
    OS1_report_error("nxmanage.uart_manager", -EIO);
}

static void nxm_log_service(const struct nxm_slot *slot, unsigned id,
                            const char *event, int pid) {
  int written = printf("[NXMANAGE] service=%s slot=%u event=%s pid=%d "
                       "generation=%u mode=%u path=%s\n",
                       slot->name, id, event, pid, slot->generation,
                       slot->mode, slot->path);
  if (written < 0)
    OS1_report_error("nxmanage.uart_service", -EIO);
}

static int nxm_service_path(const char *path) {
  const char *rest = NULL;
  if (strncmp(path, "/sys/bin/", 9U) == 0)
    rest = path + 9;
  else if (strncmp(path, "/sys/services/", 14U) == 0)
    rest = path + 14;
  if (rest == NULL || *rest == '\0')
    return 0;
  for (unsigned i = 0U; i < 56U; ++i) {
    char ch = rest[i];
    if (ch == '\0')
      return 1;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return 0;
  }
  return 0;
}

static int nxm_unpack(const struct ipc_message *message,
                      struct nxm_slot *candidate) {
  const char *base = message->payload;
  unsigned offset = 0U;
  char *dest[3] = {candidate->path, candidate->args, candidate->registry};
  unsigned capacities[3] = {sizeof(candidate->path), sizeof(candidate->args),
                            sizeof(candidate->registry)};
  for (unsigned field = 0U; field < 3U; ++field) {
    unsigned length = 0U;
    if (offset >= sizeof(message->payload) ||
        nxm_string(base + offset, (unsigned)sizeof(message->payload) - offset,
                   &length) != 0 ||
        length >= capacities[field])
      return -1;
    for (unsigned k = 0U; k < 64U; ++k) {
      if (k > length)
        break;
      dest[field][k] = base[offset + k];
    }
    offset += length + 1U;
  }
  if (!nxm_service_path(candidate->path) ||
      (candidate->registry[0] != '\0' &&
       strncmp(candidate->registry, "srv.", 4U) != 0))
    return -1;
  return offset == sizeof(message->payload) || base[offset] == '\0' ? 0 : -1;
}

static int nxm_spawn(const struct nxm_slot *slot) {
  if (!slot->auth_set)
    return -EPERM;
  if (slot->args[0] == '\0')
    return (int)spawn_caps(slot->path, PLVL_ROOT, slot->caps);
  if (slot->caps != CAP_ALL)
    return -EINVAL; /* argumentful capability-spawn needs an argv-aware API */
  char args[sizeof(slot->args)];
  for (unsigned i = 0U; i < sizeof(args); ++i)
    args[i] = slot->args[i];
  char *argv[3] = {(char *)slot->path, NULL, NULL};
  unsigned argc = 1U, offset = 0U;
  for (unsigned i = 0U; i < sizeof(args); ++i) {
    char ch = args[i];
    if (ch == ' ' || ch == '\0') {
      if (offset < i) {
        if (argc >= 3U)
          return -EINVAL;
        argv[argc++] = args + offset;
        args[i] = '\0';
      }
      offset = i + 1U;
    }
    if (ch == '\0')
      break;
  }
  return spawn_args(slot->path, (int)argc, argv);
}

/* One stable registry object per canonical service name; a reader cannot
 * observe a half-updated multi-key service record. The old numeric slot and
 * optional legacy discovery key remain readable, never authoritative. */
static void nxm_publish(const struct nxm_slot *slot, unsigned id) {
  char namekey[64], slotkey[40], record[48], pidbuf[24];
  if (!slot->named)
    return;
  int k = snprintf(namekey, sizeof(namekey), "system.manage.service.%s",
                   slot->name);
  int i = snprintf(slotkey, sizeof(slotkey), "system.manage.slot.%02u", id);
  unsigned status = slot->queued          ? 2U
                    : slot->once_finished ? 3U
                    : slot->pid > 0       ? 1U
                                          : 0U;
  int r = snprintf(record, sizeof(record), "%d:%u:%u:%u", slot->pid,
                   slot->generation, slot->mode, status);
  int n = snprintf(pidbuf, sizeof(pidbuf), "%d", slot->pid);
  if (k < 0 || k >= (int)sizeof(namekey) || i < 0 ||
      i >= (int)sizeof(slotkey) || r < 0 || r >= (int)sizeof(record) || n < 0 ||
      n >= (int)sizeof(pidbuf))
    return;
  int first = OS1_registry_object_set(namekey, record);
  int second = OS1_registry_object_set(slotkey, record);
  int third = slot->registry[0] != '\0'
                  ? OS1_registry_object_set(slot->registry, pidbuf)
                  : 0;
  if (first != 0)
    OS1_report_error("nxmanage.service_publish", first);
  if (second != 0)
    OS1_report_error("nxmanage.slot_publish", second);
  if (third != 0)
    OS1_report_error("nxmanage.pid_publish", third);
}

static int nxm_start_slot(struct nxm_slot *slot, unsigned id, uint64_t now_ms) {
  int pid = nxm_spawn(slot);
  if (pid <= 0)
    return pid;
  slot->pid = pid;
  slot->started_ms = now_ms;
  ++slot->generation;
  if (slot->mode == NXMANAGE_MODE_ONCE ||
      slot->mode == NXMANAGE_MODE_ASYNC_ONCE) {
    uint64_t budget = slot->mode == NXMANAGE_MODE_ONCE
                          ? NXMANAGE_ONCE_TIMEOUT_MS
                          : NXMANAGE_ASYNC_TIMEOUT_MS;
    slot->deadline_ms =
        now_ms > UINT64_MAX - budget ? UINT64_MAX : now_ms + budget;
  }
  slot->last_result = pid;
  slot->active = 1;
  nxm_publish(slot, id);
  nxm_log_service(slot, id, "started", pid);
  return pid;
}

static int nxm_matching(const struct nxm_slot *old,
                        const struct nxm_slot *candidate) {
  return old->mode == candidate->mode &&
         old->caps == candidate->caps &&
         strcmp(old->path, candidate->path) == 0 &&
         strcmp(old->args, candidate->args) == 0 &&
         strcmp(old->registry, candidate->registry) == 0;
}

/* Authenticated service identity reservation. Service names are not
 * inferred from path basenames and are never hardcoded in this daemon.
 * A live slot is immutable; distinct slots cannot share registry identity. */
static int nxm_name(struct nxm_state *state,
                    const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  if (id >= NXMANAGE_SLOTS || message->data2 != NXMANAGE_PROTO_VERSION)
    return -EINVAL;
  const char *name = message->payload;
  unsigned len = 0U;
  for (; len < NXMANAGE_NAME_MAX; ++len) {
    char ch = name[len];
    if (ch == '\0')
      break;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
          ch == '-'))
      return -EINVAL;
  }
  if (len == 0U || len >= NXMANAGE_NAME_MAX)
    return -EINVAL;
  for (unsigned n = 0U; n < NXMANAGE_SLOTS; ++n) {
    if (n != id && state->slots[n].named &&
        strcmp(state->slots[n].name, name) == 0)
      return -EEXIST;
  }
  struct nxm_slot *slot = &state->slots[id];
  if (slot->named)
    return strcmp(slot->name, name) == 0 ? 0 : -EPERM;
  for (unsigned i = 0U; i < NXMANAGE_NAME_MAX; ++i) {
    slot->name[i] = name[i];
    if (name[i] == '\0')
      break;
  }
  slot->named = 1;
  return 0;
}

static int nxm_library_name_valid(const char *name, unsigned *length) {
  if (name == NULL || length == NULL) return -EINVAL;
  for (unsigned i = 0U; i < NXMANAGE_LIBRARY_NAME_MAX; ++i) {
    char ch = name[i];
    if (ch == '\0') {
      if (i == 0U) return -EINVAL;
      *length = i;
      return 0;
    }
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '_' || ch == '-')) return -EINVAL;
  }
  return -ENAMETOOLONG;
}

static int nxm_library_path_valid(const char *path) {
  if (path == NULL || strncmp(path, "/lib/", 5U) != 0 || path[5] == '\0')
    return 0;
  for (unsigned i = 5U; i < NXMANAGE_LIBRARY_PATH_MAX; ++i) {
    char ch = path[i];
    if (ch == '\0') return 1;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
          ch == '.')) return 0;
  }
  return 0;
}

static int nxm_library_abi_valid(const char *abi) {
  if (abi == NULL || abi[0] == '\0') return 0;
  for (unsigned i = 0U; i < NXMANAGE_LIBRARY_ABI_MAX; ++i) {
    char ch = abi[i];
    if (ch == '\0') return 1;
    if (!((ch >= '0' && ch <= '9') || ch == '.')) return 0;
  }
  return 0;
}

static int nxm_library_unpack(const struct ipc_message *message,
                              struct nxm_library *candidate) {
  const char *base = message->payload;
  unsigned first = 0U;
  if (nxm_string(base, (unsigned)sizeof(message->payload), &first) != 0 ||
      first >= sizeof(candidate->path)) return -EINVAL;
  unsigned second = 0U;
  unsigned offset = first + 1U;
  if (offset >= sizeof(message->payload) ||
      nxm_string(base + offset, (unsigned)sizeof(message->payload) - offset,
                 &second) != 0 || second >= sizeof(candidate->abi)) return -EINVAL;
  if (memcpy(candidate->path, base, first + 1U) != candidate->path ||
      memcpy(candidate->abi, base + offset, second + 1U) != candidate->abi)
    return -EIO;
  if (!nxm_library_path_valid(candidate->path) ||
      !nxm_library_abi_valid(candidate->abi)) return -EINVAL;
  return 0;
}

static int nxm_library_name(struct nxm_state *state,
                            const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  unsigned length = 0U;
  if (id >= NXMANAGE_LIBRARY_SLOTS ||
      message->data2 != NXMANAGE_PROTO_VERSION ||
      nxm_library_name_valid(message->payload, &length) != 0) return -EINVAL;
  for (unsigned i = 0U; i < NXMANAGE_LIBRARY_SLOTS; ++i) {
    if (i != id && state->libraries[i].named &&
        strcmp(state->libraries[i].name, message->payload) == 0) return -EEXIST;
  }
  struct nxm_library *library = &state->libraries[id];
  if (library->named)
    return strcmp(library->name, message->payload) == 0 ? 0 : -EPERM;
  for (unsigned i = 0U; i <= length && i < NXMANAGE_LIBRARY_NAME_MAX; ++i)
    library->name[i] = message->payload[i];
  library->named = 1;
  return 0;
}

static int nxm_library_publish(const struct nxm_library *library) {
  char key[64];
  char value[64];
  int k = snprintf(key, sizeof(key), "%s%s", NXMANAGE_LIBRARY_REGISTRY_PREFIX,
                   library->name);
  int v = snprintf(value, sizeof(value), "%s|%s", library->path, library->abi);
  if (k <= 0 || k >= (int)sizeof(key) || v <= 0 || v >= (int)sizeof(value))
    return -EOVERFLOW;
  return OS1_registry_object_set(key, value);
}

static int nxm_library_register(struct nxm_state *state,
                                const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  if (id >= NXMANAGE_LIBRARY_SLOTS ||
      message->data2 != NXMANAGE_PROTO_VERSION) return -EINVAL;
  struct nxm_library *library = &state->libraries[id];
  if (!library->named) return -EPERM;
  struct nxm_library candidate = {0};
  if (nxm_library_unpack(message, &candidate) != 0) return -EINVAL;
  if (library->active) {
    if (strcmp(library->path, candidate.path) != 0 ||
        strcmp(library->abi, candidate.abi) != 0) return -EPERM;
    return 0;
  }
  unsigned char probe = 0U;
  int present = file_read(candidate.path, &probe, 1U, 0);
  if (present != 1) return present < 0 ? present : -ENOENT;
  if (memcpy(library->path, candidate.path, sizeof(library->path)) !=
          library->path ||
      memcpy(library->abi, candidate.abi, sizeof(library->abi)) != library->abi)
    return -EIO;
  int published = nxm_library_publish(library);
  if (published != 0) return published;
  library->active = 1;
  int logged = printf("[NXMANAGE] library=%s path=%s abi=%s registered\n",
                      library->name, library->path, library->abi);
  if (logged < 0) OS1_report_error("nxmanage.uart_library", -EIO);
  return 0;
}

/* The authenticated initializer selects a capability mask for each slot.
 * NAME must precede AUTH; REGISTER consumes the immutable slot authority. */
static int nxm_auth(struct nxm_state *state,
                    const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  if (id >= NXMANAGE_SLOTS) return -EINVAL;
  struct nxm_slot *slot = &state->slots[id];
  if (!slot->named || slot->active) return -EPERM;
  uint64_t requested = message->data2;
  if (requested != CAP_ALL &&
      requested != (CAP_ALL | CAP_WINDOW_MANAGE)) return -EPERM;
  if (slot->auth_set && slot->caps != (uint32_t)requested) return -EPERM;
  slot->caps = (uint32_t)requested;
  slot->auth_set = 1U;
  return 0;
}

static int nxm_register(struct nxm_state *state,
                        const struct ipc_message *message) {
  unsigned id = (unsigned)message->data1;
  uint32_t mode = (uint32_t)message->data2;
  if (id >= NXMANAGE_SLOTS ||
      (mode != NXMANAGE_MODE_LOCK && mode != NXMANAGE_MODE_ONCE &&
       mode != NXMANAGE_MODE_ASYNC_ONCE && mode != NXMANAGE_MODE_START_ONCE))
    return -EINVAL;
  struct nxm_slot candidate = {0};
  candidate.mode = mode;
  if (nxm_unpack(message, &candidate) != 0)
    return -EINVAL;
  struct nxm_slot *slot = &state->slots[id];
  if (!slot->named || !slot->auth_set)
    return -EPERM;
  candidate.caps = slot->caps;
  candidate.auth_set = slot->auth_set;
  for (unsigned i = 0U; i < NXMANAGE_NAME_MAX; ++i)
    candidate.name[i] = slot->name[i];
  candidate.named = 1;
  if (slot->active) {
    if (!nxm_matching(slot, &candidate))
      return -EPERM;
    if (mode == NXMANAGE_MODE_ONCE && !slot->once_finished) {
      slot->ack_target = state->init_pid;
      return 0; /* completion ACK deferred without blocking PID1 */
    }
    return slot->once_finished ? slot->last_result : slot->pid;
  }
  *slot = candidate;
  slot->active = 1; /* lock remains registered even if spawn fails */
  slot->backoff_ms = NXMANAGE_BASE_MS;
  int result = nxm_start_slot(slot, id, os1_mono_ns() / UINT64_C(1000000));
  if (result > 0 && mode == NXMANAGE_MODE_ONCE) {
    slot->ack_target = state->init_pid;
    return 0; /* ACK on actual exit, never on spawn-only */
  }
  if (result <= 0 && mode == NXMANAGE_MODE_LOCK) {
    slot->queued = 1;
    slot->due_ms = os1_mono_ns() / UINT64_C(1000000) + slot->backoff_ms;
    nxm_publish(slot, id);
    OS1_report_error("nxmanage.spawn_retry", result);
  }
  return result;
}

/* One-time authenticated capability grant: a public name is NEVER enough
 * to obtain a private management SEND right. PID is kernel stamped on IPC.
 * The new init instance must have allocated a handle table before HELLO. */
static void nxm_auth_init(struct nxm_state *state, int send_handle) {
  for (unsigned work = 0U; work < NXMANAGE_QUEUE_BUDGET; ++work) {
    struct ipc_message hello = {0};
    if (OS1low_ipc_try_recv(-1, &hello) != 0)
      return;
    if (hello.from != state->init_pid || hello.type != NXMANAGE_REQ_HELLO ||
        hello.data1 != NXMANAGE_PROTO_VERSION) {
      OS1_report_error("nxmanage.unauthorized_hello", -EPERM);
      continue;
    }
    long granted = OS1low_cap_grant(hello.from, send_handle, OS1_RIGHT_WRITE);
    struct ipc_message reply = {0};
    reply.type = NXMANAGE_REP_HELLO;
    reply.data1 = (uint64_t)granted;
    reply.data2 = NXMANAGE_PROTO_VERSION;
    long delivered = OS1low_ipc_send(hello.from, &reply);
    if (delivered != 0)
      OS1_report_error("nxmanage.authority_grant",
                       (int)(delivered < 0 ? delivered : -EIO));
  }
}

static void nxm_reply_kind(int target_pid, unsigned slot, int result,
                           uint32_t kind) {
  struct ipc_message response = {0};
  /* Protocol request/reply types are fixed positive 32-bit enum tags. */
  if (kind > INT32_MAX)
    return;
  response.type = (int)kind;
  response.data1 = slot;
  response.data2 = (uint64_t)(int64_t)result;
  long sent = OS1low_ipc_send(target_pid, &response);
  if (sent != 0)
    OS1_report_error("nxmanage.manager_reply", (int)(sent < 0 ? sent : -EIO));
}

static void nxm_reply(int target_pid, unsigned slot, int result) {
  nxm_reply_kind(target_pid, slot, result, NXMANAGE_REP_REGISTER);
}

static void nxm_drain(struct nxm_state *state, int port) {
  for (unsigned work = 0U; work < NXMANAGE_QUEUE_BUDGET; ++work) {
    struct ipc_message request = {0};
    long result = OS1_port_try_recv(port, &request);
    if (result == -EAGAIN)
      return;
    if (result != (long)sizeof(request)) {
      OS1_report_error("nxmanage.buffered_receive",
                       (int)(result < 0 ? result : -EIO));
      return;
    }
    if (request.from != state->init_pid) {
      OS1_report_error("nxmanage.unauthorized_request", -EPERM);
      continue;
    }
    if (request.type == NXMANAGE_REQ_NAME) {
      int status = nxm_name(state, &request);
      nxm_reply_kind(request.from, (unsigned)request.data1, status,
                     NXMANAGE_REP_NAME);
    } else if (request.type == NXMANAGE_REQ_AUTH) {
      int status = nxm_auth(state, &request);
      nxm_reply_kind(request.from, (unsigned)request.data1, status,
                     NXMANAGE_REP_AUTH);
    } else if (request.type == NXMANAGE_REQ_REGISTER) {
      int status = nxm_register(state, &request);
      if (status != 0)
        nxm_reply(request.from, (unsigned)request.data1, status);
    } else if (request.type == NXMANAGE_REQ_LIBRARY_NAME) {
      int status = nxm_library_name(state, &request);
      nxm_reply_kind(request.from, (unsigned)request.data1, status,
                     NXMANAGE_REP_LIBRARY_NAME);
    } else if (request.type == NXMANAGE_REQ_LIBRARY_REGISTER) {
      int status = nxm_library_register(state, &request);
      nxm_reply_kind(request.from, (unsigned)request.data1, status,
                     NXMANAGE_REP_LIBRARY_REGISTER);
    } else {
      OS1_report_error("nxmanage.unknown_operation", -EPERM);
    }
  }
}

static void nxm_schedule(struct nxm_slot *slot, uint64_t now_ms) {
  uint64_t uptime = now_ms - slot->started_ms;
  if (uptime >= NXMANAGE_STABLE_MS)
    slot->backoff_ms = NXMANAGE_BASE_MS;
  else if (slot->backoff_ms < NXMANAGE_CEILING_MS) {
    uint32_t doubled = slot->backoff_ms * 2U;
    slot->backoff_ms =
        doubled < NXMANAGE_CEILING_MS ? doubled : NXMANAGE_CEILING_MS;
  }
  slot->pid = 0;
  slot->queued = 1;
  slot->due_ms = now_ms + slot->backoff_ms;
}

/* Non-blocking completion of a real ONE-SHOT child. PID1 continues to
 * consume management messages, scan all locks and run the user-mode work
 * quantum while a dependency or a long mdebug suite executes. */
static void nxm_check_once(struct nxm_slot *slot, unsigned id,
                           uint64_t now_ms) {
  if (slot->pid <= 0 || slot->once_finished)
    return;
  int status = 0;
  int pid = slot->pid;
  int polled = waitpid(pid, &status, WNOHANG);
  if (slot->mode == NXMANAGE_MODE_START_ONCE) {
    if (polled == 0)
      return; /* user controls this program's lifetime, not a deadline */
    slot->pid = 0;
    slot->once_finished = 1;
    slot->last_result = polled == pid ? pid : -EIO;
    nxm_publish(slot, id);
    return; /* never respawn and never report a normal close as an error */
  }
  if (polled == 0 && now_ms < slot->deadline_ms)
    return;
  int outcome = -EIO;
  if (polled == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    outcome = pid;
  if (polled == 0) {
    int killed = kill_process(pid);
    if (killed < 0)
      OS1_report_error("nxmanage.oneshot_kill", killed);
    OS1_report_error("nxmanage.oneshot_deadline", -EIO);
  }
  slot->pid = 0;
  slot->once_finished = 1;
  slot->last_result = outcome;
  slot->active = outcome > 0 ? 1 : 0;
  nxm_publish(slot, id);
  if (slot->mode == NXMANAGE_MODE_ONCE && slot->ack_target > 0) {
    nxm_reply(slot->ack_target, id, outcome);
    slot->ack_target = 0;
  }
  if (outcome < 0)
    OS1_report_error("nxmanage.oneshot_result", outcome);
  /* Outcome, PID and generation are published to manager registry keys. */
}

static void nxm_supervise(struct nxm_state *state, uint64_t now_ms) {
  for (unsigned id = 0U; id < NXMANAGE_SLOTS; ++id) {
    struct nxm_slot *slot = &state->slots[id];
    if (!slot->active)
      continue;
    if (slot->mode != NXMANAGE_MODE_LOCK) {
      nxm_check_once(slot, id, now_ms);
      continue;
    }
    if (slot->pid > 0) {
      int status = 0;
      int result = waitpid(slot->pid, &status, WNOHANG);
      if (result == 0)
        continue;
      nxm_log_service(slot, id, "exited", slot->pid);
      nxm_schedule(slot, now_ms);
      nxm_publish(slot, id);
      OS1_report_error("nxmanage.lock_service_exited", -EIO);
    }
    if (slot->queued && now_ms >= slot->due_ms) {
      slot->queued = 0;
      if (nxm_start_slot(slot, id, now_ms) <= 0)
        nxm_schedule(slot, now_ms);
    }
  }
}

/* Run the boot initializer once per boot. PID1 owns the continuing services;
 * a failed initializer is reported and never silently run again. */
static void nxm_supervise_init(struct nxm_state *state) {
  if (state->init_finished)
    return;
  if (!state->init_started) {
    state->init_started = 1;
    state->init_pid = (int)spawn_caps(state->init_path, PLVL_ROOT, CAP_ALL);
    if (state->init_pid <= 0) {
      OS1_report_error("nxmanage.initializer_spawn", state->init_pid);
      state->init_pid = 0;
      state->init_finished = 1;
      return;
    }
    char value[20];
    int len = snprintf(value, sizeof(value), "%d", state->init_pid);
    if (len <= 0 || (size_t)len >= sizeof(value))
      OS1_report_error("nxmanage.init_pid_encode", -EOVERFLOW);
    else {
      int saved = OS1_registry_object_set("system.manage.init_pid", value);
      if (saved != 0)
        OS1_report_error("nxmanage.init_pid_publish", saved);
    }
    return;
  }
  if (state->init_pid <= 0)
    return;
  int status = 0;
  int result = waitpid(state->init_pid, &status, WNOHANG);
  if (result == 0)
    return;
  state->init_finished = 1;
  state->init_pid = 0;
  if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    OS1_report_error("nxmanage.initializer_failed", result < 0 ? result : -EIO);
}

int main(void) {
  struct nxm_state state = {0};
  if (get_pid() != 1 || nxm_boot_init_path(state.init_path) != 0 ||
      nxm_boot_project(state.project) != 0) {
    OS1_report_error("nxmanage.pid1_boot_config", -EPERM);
    return 1;
  }
  int port = OS1_port_create_private(NXMANAGE_PORT);
  int send_handle =
      port < 0 ? -1
               : (int)OS1low_handle_create(OS1_NS_PORT, NXMANAGE_PORT,
                                           OS1_RIGHT_WRITE | OS1_RIGHT_TRANSFER,
                                           OBJ_TYPE_PORT);
  int pid_saved = OS1_registry_object_set("system.manage.pid", "1");
  int system_saved =
      OS1_registry_object_set("system.manage.current", state.project);
  if (port < 0 || send_handle < 0 || pid_saved != 0 || system_saved != 0) {
    OS1_report_error("nxmanage.bind_authority", -EIO);
    return 1;
  }
  /* Registry/PORT state is authoritative. UART output is diagnostics only. */
  nxm_log_manager(&state);
  nxm_supervise_init(&state);
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    nxm_auth_init(&state, send_handle);
    nxm_drain(&state, port);
    uint64_t now_ms = os1_mono_ns() / UINT64_C(1000000);
    nxm_supervise(&state, now_ms);
    nxm_supervise_init(&state);
    OS1_sleep(NXMANAGE_POLL_MS);
  }
  return 1; /* finite service epoch exhausted */
}
