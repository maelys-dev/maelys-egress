/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "src/internal.h"
#include "common/bootstrap.h"
#include "maelys/sys/fdpass.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum broker_phase { BROKER_REQUEST, BROKER_RESPONSE, BROKER_LEASE };
typedef struct broker_slot {
    maelys_sys_socket_t *socket;
    maelys_sys_watch_t watch;
    uint64_t token, deadline;
    maelys_egress_channel_t *channel;
    struct broker_slot *reap_next;
    atomic_int reaped;
    int passed;
    enum broker_phase phase;
    unsigned status;
    size_t progress;
    unsigned char bytes[EGRESS_BOOTSTRAP_RESPONSE_SIZE];
} broker_slot_t;

struct maelys_egress_channel_broker {
    maelys_egress_connector_t *connector;
    maelys_sys_socket_t *listener;
    maelys_sys_loop_t *loop;
    maelys_sys_wakeup_t *wake;
    maelys_sys_thread_t *thread;
    maelys_sys_thread_t *reaper;
    maelys_sys_mutex_t *lock;
    maelys_sys_condition_t *condition;
    int armed, armed_ok, parent_fd, owns_path;
    maelys_egress_result_t result;
    atomic_int stopping;
    uint64_t connect_timeout, handshake_timeout, serial;
    size_t capacity;
    broker_slot_t *slots;
    broker_slot_t *reap_head, *reap_tail;
    int reaper_stopping; /* Queue and stop flag are protected by lock. */
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    maelys_sys_file_identity_t identity, parent_identity;
};

static int group_member(gid_t group) {
    if (group == getegid()) return 1;
    int count = getgroups(0, NULL);
    if (count <= 0) return 0;
    gid_t *groups = calloc((size_t)count, sizeof(*groups));
    if (!groups) return 0;
    count = getgroups(count, groups);
    int found = 0;
    for (int i = 0; i < count; ++i) if (groups[i] == group) found = 1;
    free(groups);
    return found;
}

static int parent_unchanged(maelys_egress_channel_broker_t *broker) {
    maelys_sys_file_identity_t current;
    return maelys_sys_file_path_identity(broker->parent, &current) == MAELYS_SYS_OK &&
        maelys_sys_file_identity_same(&current, &broker->parent_identity) &&
        current.owner == geteuid() && current.mode == broker->parent_identity.mode;
}

static int create_listener(maelys_egress_channel_broker_t *broker) {
    strcpy(broker->parent, broker->path);
    char *leaf = strrchr(broker->parent, '/');
    if (leaf == broker->parent) return 0;
    *leaf = '\0';
    char *canonical = realpath(broker->parent, NULL);
    int same = canonical && !strcmp(canonical, broker->parent);
    free(canonical);
    if (!same) return 0;
    broker->parent_fd = open(broker->parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat parent;
    if (broker->parent_fd < 0 || fstat(broker->parent_fd, &parent) != 0 ||
        !S_ISDIR(parent.st_mode) || parent.st_uid != geteuid()) return 0;
    mode_t mode = parent.st_mode & 07777;
    if (mode != 0700 && (mode != 02750 || !group_member(parent.st_gid))) return 0;
    if (maelys_sys_file_path_identity(broker->parent, &broker->parent_identity) != MAELYS_SYS_OK ||
        broker->parent_identity.device != parent.st_dev ||
        broker->parent_identity.inode != parent.st_ino || !parent_unchanged(broker)) return 0;
    maelys_sys_file_identity_t existing;
    if (maelys_sys_file_path_identity(broker->path, &existing) != MAELYS_SYS_ERR_NOT_FOUND)
        return 0;
    if (maelys_sys_socket_create(AF_UNIX, SOCK_STREAM, 0, &broker->listener) != MAELYS_SYS_OK)
        return 0;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, broker->path);
    if (maelys_sys_socket_bind(broker->listener, (struct sockaddr *)&address,
            (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(broker->path) + 1u)) !=
            MAELYS_SYS_OK) return 0;
    if (!parent_unchanged(broker) ||
        maelys_sys_file_path_identity(broker->path, &broker->identity) != MAELYS_SYS_OK ||
        !S_ISSOCK(broker->identity.mode) || broker->identity.owner != geteuid()) return 0;
    broker->owns_path = 1;
    struct stat socket_info;
    if (fstatat(broker->parent_fd, leaf + 1, &socket_info, AT_SYMLINK_NOFOLLOW) != 0 ||
        socket_info.st_dev != broker->identity.device || socket_info.st_ino != broker->identity.inode)
        return 0;
    if ((mode == 02750 && socket_info.st_gid != parent.st_gid &&
         chown(broker->path, (uid_t)-1, parent.st_gid) != 0) ||
        chmod(broker->path, mode == 0700 ? 0600 : 0660) != 0 || !parent_unchanged(broker) ||
        maelys_sys_socket_listen(broker->listener, 128) != MAELYS_SYS_OK) return 0;
    return 1;
}

static void reset_slot(broker_slot_t *slot) {
    memset(slot, 0, sizeof(*slot));
    atomic_init(&slot->reaped, 0);
    slot->passed = -1;
}

static void collect_slot(broker_slot_t *slot) {
    if (!slot->socket && slot->channel && atomic_load(&slot->reaped)) reset_slot(slot);
}

static void *reaper_main(void *opaque) {
    maelys_egress_channel_broker_t *broker = opaque;
    for (;;) {
        (void)maelys_sys_mutex_lock(broker->lock);
        while (!broker->reap_head && !broker->reaper_stopping)
            (void)maelys_sys_condition_wait(broker->condition, broker->lock);
        broker_slot_t *slot = broker->reap_head;
        if (slot) {
            broker->reap_head = slot->reap_next;
            if (!broker->reap_head) broker->reap_tail = NULL;
        }
        (void)maelys_sys_mutex_unlock(broker->lock);
        if (!slot) return NULL;
        /* Never join on either reactor, or with the queue lock held. The
         * slot stays occupied until this join releases the channel's thread,
         * descriptors and connector reference. No allocation per retirement. */
        maelys_egress_channel_destroy(slot->channel);
        atomic_store(&slot->reaped, 1);
        /* Do not touch slot again: the broker may now reset/reuse it. */
    }
}

static void stop_reaper(maelys_egress_channel_broker_t *broker) {
    (void)maelys_sys_mutex_lock(broker->lock);
    broker->reaper_stopping = 1;
    (void)maelys_sys_condition_broadcast(broker->condition);
    (void)maelys_sys_mutex_unlock(broker->lock);
}

static void drop_slot(maelys_egress_channel_broker_t *broker, broker_slot_t *slot) {
    if (!slot->socket) return; /* Already free or queued for destruction. */
    if (slot->watch) (void)maelys_sys_loop_unwatch(broker->loop, slot->watch);
    slot->watch = 0u;
    (void)maelys_sys_fd_close(&slot->passed);
    (void)maelys_sys_socket_release(&slot->socket);
    if (!slot->channel) { reset_slot(slot); return; }
    egress_channel_stop(slot->channel);
    (void)maelys_sys_mutex_lock(broker->lock);
    slot->reap_next = NULL;
    if (broker->reap_tail) broker->reap_tail->reap_next = slot;
    else broker->reap_head = slot;
    broker->reap_tail = slot;
    (void)maelys_sys_condition_broadcast(broker->condition);
    (void)maelys_sys_mutex_unlock(broker->lock);
}

static int retry_io(maelys_sys_result_t result) {
    return result == MAELYS_SYS_ERR_WOULD_BLOCK ||
        (result == MAELYS_SYS_ERR_OS && errno == EINTR);
}

static int answer(maelys_egress_channel_broker_t *broker, broker_slot_t *slot, unsigned status) {
    if (status == EGRESS_BOOTSTRAP_OK) {
        maelys_egress_result_t result = maelys_egress_channel_create(broker->connector,
            broker->connect_timeout, &slot->channel, &slot->passed, NULL);
        if (result != MAELYS_EGRESS_OK) status = result == MAELYS_EGRESS_ERR_MEMORY ?
            EGRESS_BOOTSTRAP_RESOURCE : EGRESS_BOOTSTRAP_INTERNAL;
    }
    slot->phase = BROKER_RESPONSE;
    slot->status = status;
    slot->progress = 0u;
    egress_bootstrap_response(status, broker->connect_timeout, slot->bytes);
    return maelys_sys_loop_modify(broker->loop, slot->watch,
        MAELYS_SYS_INTEREST_READ | MAELYS_SYS_INTEREST_WRITE) == MAELYS_SYS_OK;
}

static int serve_slot(maelys_egress_channel_broker_t *broker, broker_slot_t *slot, unsigned events) {
    int fd = maelys_sys_socket_native_fd(slot->socket);
    if (events & (MAELYS_SYS_EVENT_READ | MAELYS_SYS_EVENT_HUP | MAELYS_SYS_EVENT_ERROR)) {
        unsigned char extra;
        size_t received = 0u, count = 0u;
        unsigned flags = 0u;
        int request = slot->phase == BROKER_REQUEST;
        maelys_sys_result_t result = maelys_sys_fd_stream_receive(fd,
            request ? slot->bytes + slot->progress : &extra,
            request ? EGRESS_BOOTSTRAP_REQUEST_SIZE - slot->progress : 1u,
            &received, NULL, 0u, &count, &flags);
        if (!retry_io(result)) {
            if (result != MAELYS_SYS_OK) return 0;
            if (!request) return 0; /* No post-request data or control, even zero-byte rights. */
            if (flags || !received) return answer(broker, slot, EGRESS_BOOTSTRAP_MALFORMED);
            slot->progress += received;
            if (slot->progress == EGRESS_BOOTSTRAP_REQUEST_SIZE)
                return answer(broker, slot,
                    egress_bootstrap_decode_request(slot->bytes, slot->progress));
        }
    }
    if (slot->phase == BROKER_RESPONSE && (events & MAELYS_SYS_EVENT_WRITE)) {
        size_t sent = 0u;
        maelys_sys_result_t result = maelys_sys_fd_stream_send(fd,
            slot->bytes + slot->progress, EGRESS_BOOTSTRAP_RESPONSE_SIZE - slot->progress,
            slot->progress ? -1 : slot->passed, &sent);
        if (retry_io(result)) return 1;
        if (result != MAELYS_SYS_OK || !sent) return 0;
        slot->progress += sent;
        /* Positive progress transfers rights once. Keep no duplicate while
         * awaiting the remaining bytes; failure still destroys the channel. */
        (void)maelys_sys_fd_close(&slot->passed);
        if (slot->progress == EGRESS_BOOTSTRAP_RESPONSE_SIZE) {
            if (slot->status != EGRESS_BOOTSTRAP_OK) return 0;
            slot->phase = BROKER_LEASE;
            if (maelys_sys_loop_modify(broker->loop, slot->watch, MAELYS_SYS_INTEREST_READ) !=
                MAELYS_SYS_OK) return 0;
        }
    }
    return 1;
}

static void accept_one(maelys_egress_channel_broker_t *broker) {
    maelys_sys_socket_t *socket_handle = NULL;
    if (maelys_sys_socket_accept(broker->listener, NULL, NULL, &socket_handle) != MAELYS_SYS_OK)
        return;
    size_t index = 0u;
    for (; index < broker->capacity; ++index) {
        collect_slot(&broker->slots[index]);
        if (!broker->slots[index].socket && !broker->slots[index].channel) break;
    }
    if (index == broker->capacity) {
        unsigned char response[EGRESS_BOOTSTRAP_RESPONSE_SIZE];
        egress_bootstrap_response(EGRESS_BOOTSTRAP_BUSY, 0u, response);
        size_t sent = 0u;
        /* No slot or channel for excess peers. A fresh output queue
         * normally accepts the fixed reply; on failure close, never block. */
        (void)maelys_sys_fd_stream_send(maelys_sys_socket_native_fd(socket_handle),
            response, sizeof(response), -1, &sent);
        (void)maelys_sys_socket_release(&socket_handle);
        return;
    }
    broker_slot_t *slot = &broker->slots[index];
    slot->socket = socket_handle;
    slot->token = (++broker->serial << 16u) | (index + 2u);
    if (maelys_sys_deadline_after(broker->handshake_timeout, &slot->deadline) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(broker->loop, maelys_sys_socket_native_fd(socket_handle),
            MAELYS_SYS_INTEREST_READ, slot->token, &slot->watch) != MAELYS_SYS_OK)
        drop_slot(broker, slot);
}

static void remove_path(maelys_egress_channel_broker_t *broker) {
    if (!broker->owns_path) return;
    if (!parent_unchanged(broker) ||
        maelys_sys_file_unlink_same(broker->path, &broker->identity) != MAELYS_SYS_OK)
        broker->result = MAELYS_EGRESS_ERR_IO;
    broker->owns_path = 0;
}

static void *broker_main(void *opaque) {
    maelys_egress_channel_broker_t *broker = opaque;
    maelys_sys_watch_t listener = 0u, wake = 0u;
    int ready = maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &broker->loop) == MAELYS_SYS_OK &&
        maelys_sys_loop_watch_fd(broker->loop, maelys_sys_socket_native_fd(broker->listener),
            MAELYS_SYS_INTEREST_READ, 0u, &listener) == MAELYS_SYS_OK &&
        maelys_sys_loop_watch_fd(broker->loop, maelys_sys_wakeup_fd(broker->wake),
            MAELYS_SYS_INTEREST_READ, 1u, &wake) == MAELYS_SYS_OK;
    (void)maelys_sys_mutex_lock(broker->lock);
    broker->result = ready ? MAELYS_EGRESS_OK : MAELYS_EGRESS_ERR_IO;
    broker->armed_ok = ready;
    broker->armed = 1;
    (void)maelys_sys_condition_broadcast(broker->condition);
    (void)maelys_sys_mutex_unlock(broker->lock);
    while (ready && !atomic_load(&broker->stopping) && egress_connector_is_running(broker->connector)) {
        uint64_t now = 0u;
        if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK) { broker->result = MAELYS_EGRESS_ERR_IO; break; }
        uint64_t deadline = now + 50u;
        for (size_t i = 0u; i < broker->capacity; ++i) {
            broker_slot_t *slot = &broker->slots[i];
            collect_slot(slot);
            if (!slot->socket || slot->phase == BROKER_LEASE) continue;
            if (now >= slot->deadline) drop_slot(broker, slot);
            else if (slot->deadline < deadline) deadline = slot->deadline;
        }
        maelys_sys_event_t events[64];
        size_t count = 0u;
        maelys_sys_step_result_t stepped;
        if (maelys_sys_loop_step(broker->loop, deadline, events, 64u, &count, &stepped) != MAELYS_SYS_OK) {
            broker->result = MAELYS_EGRESS_ERR_IO; break;
        }
        for (size_t i = 0u; i < count && !atomic_load(&broker->stopping); ++i) {
            uint64_t token = events[i].token;
            if (token == 1u) { atomic_store(&broker->stopping, 1); break; }
            if (token == 0u) {
                if (!parent_unchanged(broker)) { broker->result = MAELYS_EGRESS_ERR_IO; ready = 0; break; }
                accept_one(broker);
                continue;
            }
            size_t index = (size_t)(token & UINT64_C(0xffff)) - 2u;
            if (index >= broker->capacity) continue;
            broker_slot_t *slot = &broker->slots[index];
            if (!slot->socket || slot->token != token) continue;
            if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK ||
                (slot->phase != BROKER_LEASE && now >= slot->deadline) ||
                !serve_slot(broker, slot, events[i].flags)) drop_slot(broker, slot);
        }
    }
    for (size_t i = 0u; i < broker->capacity; ++i) drop_slot(broker, &broker->slots[i]);
    if (listener) (void)maelys_sys_loop_unwatch(broker->loop, listener);
    if (wake) (void)maelys_sys_loop_unwatch(broker->loop, wake);
    (void)maelys_sys_loop_destroy(&broker->loop);
    (void)maelys_sys_socket_release(&broker->listener);
    remove_path(broker);
    stop_reaper(broker);
    return NULL;
}

maelys_egress_result_t maelys_egress_channel_broker_destroy(
    maelys_egress_channel_broker_t *broker, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!broker) return MAELYS_EGRESS_OK;
    if (broker->thread) {
        atomic_store(&broker->stopping, 1);
        (void)maelys_sys_wakeup_signal(broker->wake);
        (void)maelys_sys_thread_join(&broker->thread, NULL);
    }
    if (broker->reaper) {
        stop_reaper(broker);
        (void)maelys_sys_thread_join(&broker->reaper, NULL);
    }
    (void)maelys_sys_socket_release(&broker->listener);
    remove_path(broker);
    (void)maelys_sys_fd_close(&broker->parent_fd);
    maelys_sys_wakeup_destroy(broker->wake);
    maelys_sys_condition_destroy(broker->condition);
    maelys_sys_mutex_destroy(broker->lock);
    maelys_egress_connector_release(broker->connector);
    maelys_egress_result_t result = broker->result;
    if (result != MAELYS_EGRESS_OK) egress_set_error(out_error, "broker failed or its socket path changed");
    free(broker->slots);
    free(broker);
    return result;
}

maelys_egress_result_t maelys_egress_channel_broker_create(
    maelys_egress_connector_t *connector, const char *path,
    uint64_t connect_timeout_ms, uint64_t handshake_timeout_ms, size_t max_clients,
    maelys_egress_channel_broker_t **out_broker, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_broker) *out_broker = NULL;
    if (!connector || !out_broker || !egress_bootstrap_path_valid(path) ||
        strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
        !connect_timeout_ms || connect_timeout_ms > 600000u ||
        !handshake_timeout_ms || handshake_timeout_ms > 60000u || !max_clients || max_clients > 4096u) {
        egress_set_error(out_error, "broker needs a canonical Unix path, connector and bounded deadlines/capacity");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (!egress_connector_is_running(connector)) return MAELYS_EGRESS_ERR_STATE;
    maelys_egress_channel_broker_t *broker = calloc(1u, sizeof(*broker));
    if (!broker) return MAELYS_EGRESS_ERR_MEMORY;
    broker->parent_fd = -1;
    broker->result = MAELYS_EGRESS_ERR_IO;
    atomic_init(&broker->stopping, 0);
    broker->slots = calloc(max_clients, sizeof(*broker->slots));
    if (!broker->slots) { free(broker); return MAELYS_EGRESS_ERR_MEMORY; }
    broker->capacity = max_clients;
    for (size_t i = 0u; i < max_clients; ++i) reset_slot(&broker->slots[i]);
    broker->connector = connector;
    maelys_egress_connector_retain(connector);
    strcpy(broker->path, path);
    broker->connect_timeout = connect_timeout_ms;
    broker->handshake_timeout = handshake_timeout_ms;
    if (!create_listener(broker) || maelys_sys_wakeup_create(&broker->wake) != MAELYS_SYS_OK ||
        maelys_sys_mutex_create(&broker->lock) != MAELYS_SYS_OK ||
        maelys_sys_condition_create(&broker->condition) != MAELYS_SYS_OK ||
        maelys_sys_thread_create("egress-reaper", reaper_main, broker, &broker->reaper) != MAELYS_SYS_OK ||
        maelys_sys_thread_create("egress-broker", broker_main, broker, &broker->thread) != MAELYS_SYS_OK)
        goto fail;
    (void)maelys_sys_mutex_lock(broker->lock);
    while (!broker->armed) (void)maelys_sys_condition_wait(broker->condition, broker->lock);
    int ready = broker->armed_ok;
    (void)maelys_sys_mutex_unlock(broker->lock);
    if (!ready) goto fail;
    *out_broker = broker;
    return MAELYS_EGRESS_OK;
fail:
    (void)maelys_egress_channel_broker_destroy(broker, NULL);
    egress_set_error(out_error, "cannot create broker: check socket parent ownership, mode, path and resources");
    return MAELYS_EGRESS_ERR_IO;
}
