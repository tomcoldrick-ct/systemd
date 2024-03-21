/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-bus.h"

#include "bus-get-properties.h"
#include "bus-label.h"
#include "bus-log-control-api.h"
#include "bus-polkit.h"
#include "common-signal.h"
#include "discover-image.h"
#include "env-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "json.h"
#include "main-func.h"
#include "memfd-util.h"
#include "missing_capability.h"
#include "mkdir-label.h"
#include "os-util.h"
#include "process-util.h"
#include "service-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "string-table.h"
#include "user-util.h"

typedef struct Manager {
        sd_event *event;
        sd_bus *bus;

        Hashmap *targets;

        uint64_t current_job_id;
        Hashmap *jobs;

        Hashmap *polkit_registry;

        sd_event_source *notify_event;
} Manager;

typedef enum TargetClass {
        /* These should try to match ImageClass from src/basic/os-util.h */
        TARGET_MACHINE = IMAGE_MACHINE,
        TARGET_PORTABLE = IMAGE_PORTABLE,
        TARGET_SYSEXT = IMAGE_SYSEXT,
        TARGET_CONFEXT = IMAGE_CONFEXT,

        /* sysupdate-specific classes */
        TARGET_HOST,
        TARGET_COMPONENT,

        _TARGET_CLASS_MAX,
        _TARGET_CLASS_INVALID = -EINVAL,
} TargetClass;
assert_cc(_IMAGE_CLASS_MAX == 4);

typedef struct Target {
        Manager *manager;

        TargetClass class;
        char *name;
        char *path;
        char *version;
        char *appstream;

        char *id;
        ImageType image_type;
        bool busy;
} Target;

typedef enum JobType {
        JOB_LIST,
        JOB_DESCRIBE,
        JOB_CHECK_NEW,
        JOB_UPDATE,
        JOB_VACUUM,
        _JOB_TYPE_MAX,
        _JOB_TYPE_INVALID = -EINVAL,
} JobType;

typedef struct Job Job;

typedef int (*JobComplete)(sd_bus_message *, const Job *, JsonVariant *, sd_bus_error *);

struct Job {
        Manager *manager;
        Target *target;

        uint64_t id;
        char *object_path;

        JobType type;
        bool offline;
        char *version; /* Passed into sysupdate for JOB_DESCRIBE and JOB_UPDATE */

        unsigned progress_percent;

        sd_event_source *child;
        int stdout_fd;
        int status_errno;
        unsigned n_cancelled;

        JsonVariant *json;

        JobComplete complete_cb;
        sd_bus_message *dbus_msg;
        bool detach_on_ready;
};

static const char* const target_class_table[_TARGET_CLASS_MAX] = {
        [TARGET_MACHINE] = "machine",
        [TARGET_PORTABLE] = "portable",
        [TARGET_SYSEXT] = "extension",
        [TARGET_CONFEXT] = "confext",
        [TARGET_COMPONENT] = "component",
        [TARGET_HOST] = "host",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(target_class, TargetClass);

static const char* const job_type_table[_JOB_TYPE_MAX] = {
        [JOB_LIST] = "list",
        [JOB_DESCRIBE] = "describe",
        [JOB_CHECK_NEW] = "check-new",
        [JOB_UPDATE] = "update",
        [JOB_VACUUM] = "vacuum",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(job_type, JobType);

static Job *job_free(Job *j) {
        if (!j)
                return NULL;

        if (j->manager)
                hashmap_remove(j->manager->jobs, &j->id);

        free(j->object_path);
        free(j->version);

        json_variant_unref(j->json);

        sd_bus_message_unref(j->dbus_msg);

        sd_event_source_unref(j->child);
        safe_close(j->stdout_fd);

        return mfree(j);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Job*, job_free);
DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(job_hash_ops, uint64_t, uint64_hash_func, uint64_compare_func,
                                      Job, job_free);

static int job_new(JobType type, Target *t, sd_bus_message *msg, JobComplete cb, Job **ret) {
        _cleanup_(job_freep) Job *j = NULL;
        int r;

        assert(t);
        assert(ret);

        j = new(Job, 1);
        if (!j)
                return -ENOMEM;

        *j = (Job) {
                .type = type,
                .target = t,
                .manager = t->manager,
                .id = t->manager->current_job_id + 1,
                .stdout_fd = -EBADF,
                .complete_cb = cb,
                .dbus_msg = sd_bus_message_ref(msg),
        };

        if (asprintf(&j->object_path, "/org/freedesktop/sysupdate1/job/_%" PRIu64, j->id) < 0)
                return -ENOMEM;

        r = hashmap_ensure_put(&t->manager->jobs, &job_hash_ops, &j->id, j);
        if (r < 0)
                return r;

        t->manager->current_job_id = j->id;

        *ret = TAKE_PTR(j);
        return 0;
}

static int job_parse_stdout(int _fd, JsonVariant **ret) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        _cleanup_close_ int fd = _fd;
        _cleanup_fclose_ FILE *f = NULL;
        struct stat st;
        int r;

        assert(ret);

        if (fstat(fd, &st) < 0)
                return log_error_errno(errno, "Failed to stat stdout fd: %m");

        assert(S_ISREG(st.st_mode));

        if (st.st_size == 0)
                return 0;

        if (lseek(fd, SEEK_SET, 0) == (off_t) -1)
                return log_error_errno(errno, "Failed to seek to beginning of memfd: %m");

        f = take_fdopen(&fd, "r");
        if (!f)
                return log_error_errno(errno, "Failed to reopen memfd: %m");

        r = json_parse_file(f, "stdout", 0, &v, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to parse JSON: %m");

        *ret = TAKE_PTR(v);
        return 0;
}

static void job_on_ready(Job *j) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *msg = NULL;
        int r;

        /* Some jobs run in the background as we return the job ID to the
         * dbus caller (i.e. for the Update method). However, the worker
         * will perform some sanity-checks on startup which would be valuable
         * as dbus errors. So, we wait for the worker to signal via READY=1
         * that it has completed its sanity-checks and we should continue
         * the job in the background. */

        if (!j->detach_on_ready)
                return;

        assert(j->dbus_msg);
        msg = TAKE_PTR(j->dbus_msg);

        j->complete_cb = NULL;

        r = sd_bus_reply_method_return(msg, "suo", j->version, j->id, j->object_path);
        if (r < 0)
                log_warning_errno(r, "Failed to reply to method call: %m");
}

static void job_on_errno(Job *j, char *b) {
        _cleanup_free_ char *buf = TAKE_PTR(b);
        int r;

        if (!buf) {
                log_oom_warning();
                return;
        }

        r = parse_errno(buf);
        if (r < 0) {
                log_warning("Got invalid errno value, ignoring.");
                return;
        }

        j->status_errno = -r;

        log_debug_errno(-r, "Got errno from job %" PRIu64 ": %i (%m)", j->id, r);
}

static void job_on_progress(Job *j, char *b) {
        _cleanup_free_ char *buf = TAKE_PTR(b);
        unsigned progress;
        int r;

        if (!buf) {
                log_oom_warning();
                return;
        }

        r = safe_atou(buf, &progress);
        if (r < 0 || progress > 100) {
                log_warning("Got invalid percent value, ignoring.");
                return;
        }

        j->progress_percent = progress;
        (void) sd_bus_emit_properties_changed(j->manager->bus, j->object_path,
                                              "org.freedesktop.sysupdate1.Job",
                                              "Progress", NULL);

        log_debug("Got percentage from job %" PRIu64 ": %u%%", j->id, j->progress_percent);
}

static void job_on_version(Job *j, char *version) {
        if (!version) {
                log_oom_warning();
                return;
        }
        j->version = TAKE_PTR(version);

        log_debug("Got version from job %" PRIu64 ": %s ", j->id, j->version);
}

static int job_on_exit(sd_event_source *s, const siginfo_t *si, void *userdata) {
        Job *j = ASSERT_PTR(userdata);
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *json = NULL;
        int r;

        assert(s);
        assert(si);

        if (IN_SET(j->type, JOB_UPDATE, JOB_VACUUM)) {
                assert(j->target->busy);
                j->target->busy = false;
        }

        if (si->si_code != CLD_EXITED) {
                assert(IN_SET(si->si_code, CLD_KILLED, CLD_DUMPED));
                sd_bus_error_setf(&error, SD_BUS_ERROR_FAILED,
                                  "Job terminated abnormally with signal %s.",
                                  signal_to_string(si->si_status));
        } else if (si->si_status != EXIT_SUCCESS)
                if (j->status_errno)
                        sd_bus_error_set_errno(&error, j->status_errno);
                else
                        sd_bus_error_setf(&error, SD_BUS_ERROR_FAILED,
                                          "Job failed with exit code %i.", si->si_status);
        else {
                r = job_parse_stdout(TAKE_FD(j->stdout_fd), &json);
                if (r < 0)
                        sd_bus_error_set_errnof(&error, r, "Failed to parse JSON: %m");
        }

        r = sd_bus_emit_signal(
                        j->manager->bus,
                        "/org/freedesktop/sysupdate1",
                        "org.freedesktop.sysupdate1.Manager",
                        "JobRemoved",
                        "uoi",
                        j->id,
                        j->object_path,
                        j->status_errno ?: si->si_status);
        if (r < 0)
                log_warning_errno(r, "Cannot emit JobRemoved message, ignoring: %m");

        if (j->dbus_msg && j->complete_cb) {
                if (sd_bus_error_is_set(&error))
                        sd_bus_reply_method_error(j->dbus_msg, &error);
                else {
                        r = j->complete_cb(j->dbus_msg, j, json, &error);
                        if (r < 0 || sd_bus_error_is_set(&error))
                                sd_bus_reply_method_errno(j->dbus_msg, r, &error);
                }
        }

        job_free(j);
        return 0;
}

static inline const char* sysupdate_binary_path(void) {
        return secure_getenv("SYSTEMD_SYSUPDATE_PATH") ?: SYSTEMD_SYSUPDATE_PATH;
}

static int target_get_argument(Target *t, char **ret) {
        _cleanup_free_ char *target_arg = NULL;

        assert(t);
        assert(ret);

        if (t->class != TARGET_HOST) {
                if (t->class == TARGET_COMPONENT)
                        target_arg = strjoin("--component=", t->name);
                else if (IN_SET(t->image_type, IMAGE_DIRECTORY, IMAGE_SUBVOLUME))
                        target_arg = strjoin("--root=", t->path);
                else if (IN_SET(t->image_type, IMAGE_RAW, IMAGE_BLOCK))
                        target_arg = strjoin("--image=", t->path);
                else
                        assert_not_reached();
                if (!target_arg)
                        return -ENOMEM;
        }

        *ret = TAKE_PTR(target_arg);
        return 0;
}

static int job_start(Job *j) {
        _cleanup_close_ int stdout_fd = -EBADF;
        _cleanup_(sigkill_nowaitp) pid_t pid = 0;
        int r;

        assert(j);

        if (IN_SET(j->type, JOB_UPDATE, JOB_VACUUM) && j->target->busy)
                return -EBUSY;

        stdout_fd = memfd_new("sysupdate-stdout");
        if (stdout_fd < 0)
                return stdout_fd;

        r = safe_fork_full("(sd-sysupdate)",
                           (int[]) { -EBADF, stdout_fd, STDERR_FILENO }, NULL, 0,
                           FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_DEATHSIG|
                           FORK_REARRANGE_STDIO|FORK_LOG|FORK_REOPEN_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                /* Child */

                _cleanup_free_ char *target_arg = NULL;
                const char *cmd[] = {
                        "systemd-sysupdate",
                        "--json=short",
                        NULL, /* maybe --verify=no */
                        NULL, /* maybe --component=, --root=, or --image= */
                        NULL, /* maybe --offline */
                        NULL, /* list, check-new, update, vacuum */
                        NULL, /* maybe version (for list, update) */
                        NULL
                };
                unsigned k = 2;

                if (setenv("NOTIFY_SOCKET", "/run/systemd/sysupdate/notify", 1) < 0) {
                        log_error_errno(errno, "setenv() failed: %m");
                        _exit(EXIT_FAILURE);
                }

                if (getenv_bool("SYSTEMD_SYSUPDATE_NO_VERIFY") > 0)
                        cmd[k++] = "--verify=no"; /* For testing */

                r = setenv_systemd_exec_pid(true);
                if (r < 0)
                        log_warning_errno(r, "Failed to update $SYSTEMD_EXEC_PID, ignoring: %m");

                r = target_get_argument(j->target, &target_arg);
                if (r < 0) {
                        log_oom();
                        _exit(EXIT_FAILURE);
                }
                if (target_arg)
                        cmd[k++] = target_arg;

                if (j->offline)
                        cmd[k++] = "--offline";

                switch (j->type) {
                case JOB_LIST:
                        cmd[k++] = "list";
                        break;

                case JOB_DESCRIBE:
                        cmd[k++] = "list";
                        assert(!isempty(j->version));
                        cmd[k++] = j->version;
                        break;

                case JOB_CHECK_NEW:
                        cmd[k++] = "check-new";
                        break;

                case JOB_UPDATE:
                        cmd[k++] = "update";
                        cmd[k++] = empty_to_null(j->version);
                        break;

                case JOB_VACUUM:
                        cmd[k++] = "vacuum";
                        break;

                default:
                        assert_not_reached();
                }

                if (DEBUG_LOGGING) {
                        _cleanup_free_ char *s = NULL;

                        s = quote_command_line((char**) cmd, SHELL_ESCAPE_EMPTY);
                        if (!s) {
                                log_oom();
                                _exit(EXIT_FAILURE);
                        }

                        log_debug("Spawning worker for job %" PRIu64 ": %s", j->id, s);
                }

                execv(sysupdate_binary_path(), (char * const *) cmd);
                log_error_errno(errno, "Failed to execute systemd-sysupdate: %m");
                _exit(EXIT_FAILURE);
        }

        r = sd_event_add_child(j->manager->event, &j->child, pid, WEXITED, job_on_exit, j);
        if (r < 0)
                return r;

        r = sd_event_source_set_child_process_own(j->child, true);
        if (r < 0)
                return r;
        TAKE_PID(pid);

        j->stdout_fd = TAKE_FD(stdout_fd);

        if (IN_SET(j->type, JOB_UPDATE, JOB_VACUUM))
                j->target->busy = true;

        return 0;
}

static int job_cancel(Job *j) {
        int r;

        assert(j);

        r = sd_event_source_send_child_signal(j->child, j->n_cancelled < 3 ? SIGTERM : SIGKILL,
                                              NULL, 0);
        if (r < 0)
                return r;

        j->n_cancelled++;
        return 0;
}

static int job_method_cancel(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Job *j = ASSERT_PTR(userdata);
        const char *action;
        int r;

        assert(msg);

        switch (j->type) {
        case JOB_LIST:
        case JOB_DESCRIBE:
        case JOB_CHECK_NEW:
                action = "org.freedesktop.sysupdate1.check";
                break;

        case JOB_UPDATE:
                if (j->version)
                        action = "org.freedesktop.sysupdate1.update-to-version";
                else
                        action = "org.freedesktop.sysupdate1.update";
                break;

        case JOB_VACUUM:
                action = "org.freedesktop.sysupdate1.vacuum";
                break;

        default:
                assert_not_reached();
        }

        r = bus_verify_polkit_async(
                        msg,
                        CAP_SYS_ADMIN,
                        action,
                        /* details= */ NULL,
                        /* interactive= */ false,
                        UID_INVALID,
                        &j->manager->polkit_registry,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_cancel(j);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(msg, NULL);
}

static BUS_DEFINE_PROPERTY_GET_ENUM(job_property_get_type, job_type, JobType);

static int job_object_find(
                sd_bus *bus,
                const char *path,
                const char *iface,
                void *userdata,
                void **ret,
                sd_bus_error *error) {

        Manager *m = ASSERT_PTR(userdata);
        Job *j;
        const char *p;
        uint64_t id;
        int r;

        assert(bus);
        assert(path);
        assert(ret);

        p = startswith(path, "/org/freedesktop/sysupdate1/job/_");
        if (!p)
                return 0;

        r = safe_atou64(p, &id);
        if (r < 0 || id == 0)
                return 0;

        j = hashmap_get(m->jobs, &id);
        if (!j)
                return 0;

        *ret = j;
        return 1;
}

static int job_node_enumerator(
                sd_bus *bus,
                const char *path,
                void *userdata,
                char ***nodes,
                sd_bus_error *error) {

        _cleanup_strv_free_ char **l = NULL;
        Manager *m = ASSERT_PTR(userdata);
        Job *j;
        unsigned k = 0;

        l = new0(char*, hashmap_size(m->jobs) + 1);
        if (!l)
                return -ENOMEM;

        HASHMAP_FOREACH(j, m->jobs) {
                l[k] = strdup(j->object_path);
                if (!l[k])
                        return -ENOMEM;
                k++;
        }

        *nodes = TAKE_PTR(l);
        return 1;
}

static const sd_bus_vtable job_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Id", "u", NULL, offsetof(Job, id), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Type", "s", job_property_get_type, offsetof(Job, type), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Offline", "b", NULL, offsetof(Job, offline), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Progress", "u", NULL, offsetof(Job, progress_percent), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

        SD_BUS_METHOD("Cancel", NULL, NULL, job_method_cancel, SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_VTABLE_END
};

static const BusObjectImplementation job_object = {
        "/org/freedesktop/sysupdate1/job",
        "org.freedesktop.sysupdate1.Job",
        .fallback_vtables = BUS_FALLBACK_VTABLES({job_vtable, job_object_find}),
        .node_enumerator = job_node_enumerator,
};

static Target *target_free(Target *t) {
        if (!t)
                return NULL;

        if (t->manager)
                hashmap_remove(t->manager->targets, t->id);

        free(t->name);
        free(t->path);
        free(t->version);
        free(t->appstream);
        free(t->id);

        return mfree(t);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Target*, target_free);
DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(target_hash_ops, char, string_hash_func, string_compare_func,
                                      Target, target_free);

static int target_new(Manager *m, TargetClass class, const char *name, const char *path, Target **ret) {
        _cleanup_(target_freep) Target *t = NULL;
        int r;

        assert(m);
        assert(ret);

        t = new(Target, 1);
        if (!t)
                return -ENOMEM;

        *t = (Target) {
                .manager = m,
                .class = class,
                .image_type = _IMAGE_TYPE_INVALID,
        };

        t->name = strdup(name);
        if (!t->name)
                return -ENOMEM;

        t->path = strdup(path);
        if (!t->path)
                return -ENOMEM;

        if (class == TARGET_HOST)
                t->id = strdup("host"); /* This is what appears in the object path */
        else
                t->id = strjoin(target_class_to_string(class), ":", name);
        if (!t->id)
                return -ENOMEM;

        r = hashmap_ensure_put(&m->targets, &target_hash_ops, t->id, t);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(t);
        return 0;
}

static int cache_property_from_json(char **ret, const char *key, JsonVariant *json) {
        JsonVariant *v;
        const char *j;
        _cleanup_free_ char *s = NULL;

        v = json_variant_by_key(json, key);
        if (!v)
                return -EINVAL;

        if (json_variant_is_null(v))
                j = "";
        else
                j = json_variant_string(v);
        if (!j)
                return -EINVAL;

        s = strdup(j);
        if (!s)
                return -ENOMEM;

        *ret = TAKE_PTR(s);
        return 0;
}

static int sysupdate_run_simple(JsonVariant **ret, ...) {
        _cleanup_close_pair_ int pipe[2] = PIPE_EBADF;
        _cleanup_(sigkill_nowaitp) pid_t pid = 0;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        int r;

        r = pipe2(pipe, O_CLOEXEC);
        if (r < 0)
                return r;

        r = safe_fork_full("(sd-sysupdate)",
                           (int[]) { -EBADF, pipe[1], STDERR_FILENO },
                           NULL, 0,
                           FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_DEATHSIG|
                           FORK_REARRANGE_STDIO|FORK_WAIT|FORK_LOG|FORK_REOPEN_LOG,
                           &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                /* Child */
                va_list ap;
                char *arg;
                _cleanup_strv_free_ char **args = NULL;

                if (strv_extend(&args, "systemd-sysupdate") < 0) {
                        log_oom();
                        _exit(EXIT_FAILURE);
                }

                if (strv_extend(&args, "--json=short") < 0) {
                        log_oom();
                        _exit(EXIT_FAILURE);
                }

                va_start(ap, ret);
                while ((arg = va_arg(ap, char*)))
                        if (strv_extend(&args, arg) < 0) {
                                log_oom();
                                _exit(EXIT_FAILURE);
                        }
                va_end(ap);

                if (DEBUG_LOGGING) {
                        _cleanup_free_ char *s = NULL;

                        s = quote_command_line((char**) args, SHELL_ESCAPE_EMPTY);
                        if (!s) {
                                log_oom();
                                _exit(EXIT_FAILURE);
                        }

                        log_debug("Spawning sysupdate: %s", s);
                }

                execv(sysupdate_binary_path(), args);
                log_error_errno(errno, "Failed to execute systemd-sysupdate: %m");
                _exit(EXIT_FAILURE);
        }

        pipe[1] = safe_close(pipe[1]);
        f = take_fdopen(&pipe[0], "r");
        if (!f)
                return -errno;

        r = json_parse_file(f, "stdout", 0, &v, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to parse JSON: %m");

        *ret = TAKE_PTR(v);
        return 0;
}

static int target_ensure_properties(Target *t) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        _cleanup_free_ char *target_arg = NULL;
        int r;

        assert(t);

        if (t->version && t->appstream)
                return 0;

        r = target_get_argument(t, &target_arg);
        if (r < 0)
                return r;

        r = sysupdate_run_simple(&v, "--offline", "list", target_arg, NULL);
        if (r < 0)
                return r;

        r = cache_property_from_json(&t->version, "current", v);
        if (r < 0)
                return r;

        r = cache_property_from_json(&t->appstream, "appstream-url", v);
        if (r < 0)
                return r;

        return 0;
}

static int target_property_get_cached(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Target *t = ASSERT_PTR(userdata);
        char *s;
        int r;

        assert(bus);
        assert(reply);

        r = target_ensure_properties(t);
        if (r < 0)
                return r;

        if (streq(property, "Version"))
                s = t->version;
        else if (streq(property, "Appstream"))
                s = t->appstream;
        else
                assert_not_reached();

        return sd_bus_message_append(reply, "s", s);
}

static BUS_DEFINE_PROPERTY_GET_ENUM(target_property_get_class, target_class, TargetClass);

static int target_method_list_finish(
                sd_bus_message *msg,
                const Job *j,
                JsonVariant *json,
                sd_bus_error *error) {

        JsonVariant *v;
        _cleanup_strv_free_ char **versions = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        int r;

        assert(json);

        /* Try our best since we have the data anyway, but isn't fatal if it fails */
        (void) cache_property_from_json(&j->target->version, "current", json);
        (void) cache_property_from_json(&j->target->appstream, "appstream-url", json);

        v = json_variant_by_key(json, "all");
        if (!v)
                return -EINVAL;

        r = json_variant_strv(v, &versions);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(msg, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, versions);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int target_method_list(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Target *t = ASSERT_PTR(userdata);
        _cleanup_(job_freep) Job *j = NULL;
        bool offline;
        int r;

        assert(msg);

        r = sd_bus_message_read(msg, "b", &offline);
        if (r < 0)
                return r;

        const char *details[] = {
                "class", target_class_to_string(t->class),
                "name", t->name,
                "offline", true_false(offline),
                NULL
        };

        r = bus_verify_polkit_async(
                msg,
                CAP_SYS_ADMIN,
                "org.freedesktop.sysupdate1.check",
                details,
                /* interactive= */ false,
                UID_INVALID,
                &t->manager->polkit_registry,
                error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_new(JOB_LIST, t, msg, target_method_list_finish, &j);
        if (r < 0)
                return r;

        j->offline = offline;

        r = job_start(j);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to start job: %m");
        TAKE_PTR(j); /* Avoid job from being killed & freed */

        return 1;
}

static int target_method_describe_finish(
                sd_bus_message *msg,
                const Job *j,
                JsonVariant *json,
                sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_free_ char *contents_str = NULL;
        const char *version, *changelog = NULL;
        bool newest, available, installed, obsolete, protected;
        JsonVariant *contents;
        JsonDispatch dispatch_table[] = {
                { "version",       JSON_VARIANT_STRING,  json_dispatch_const_string,  PTR_TO_SIZE(&version),   0               },
                { "newest",        JSON_VARIANT_BOOLEAN, json_dispatch_boolean,       PTR_TO_SIZE(&newest),    0               },
                { "available",     JSON_VARIANT_BOOLEAN, json_dispatch_boolean,       PTR_TO_SIZE(&available), 0               },
                { "installed",     JSON_VARIANT_BOOLEAN, json_dispatch_boolean,       PTR_TO_SIZE(&installed), 0               },
                { "obsolete",      JSON_VARIANT_BOOLEAN, json_dispatch_boolean,       PTR_TO_SIZE(&obsolete),  0               },
                { "protected",     JSON_VARIANT_BOOLEAN, json_dispatch_boolean,       PTR_TO_SIZE(&protected), 0               },
                { "changelog-url", JSON_VARIANT_STRING,  json_dispatch_const_string,  PTR_TO_SIZE(&changelog), JSON_PERMISSIVE },
                { "contents",      JSON_VARIANT_ARRAY,   json_dispatch_variant_noref, PTR_TO_SIZE(&contents),  0               },
                {}
        };
        int r;

        assert(json);

        r = json_dispatch(json, dispatch_table, NULL, JSON_MANDATORY, NULL);
        if (r < 0)
                return r;

        assert(version);
        assert(contents);

        r = sd_bus_message_new_method_return(msg, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "{sv}", "version", "s", version);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "{sv}", "newest", "b", newest);
        if (r < 0)
                return r;
        r = sd_bus_message_append(reply, "{sv}", "available", "b", available);
        if (r < 0)
                return r;
        r = sd_bus_message_append(reply, "{sv}", "installed", "b", installed);
        if (r < 0)
                return r;
        r = sd_bus_message_append(reply, "{sv}", "obsolete", "b", obsolete);
        if (r < 0)
                return r;
        r = sd_bus_message_append(reply, "{sv}", "protected", "b", protected);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "{sv}", "changelog-url", "s", changelog);
        if (r < 0)
                return r;

        /* "contents" is just a serialized table that we want to present from updatectl.
         * So, let's make this a "private" key and just send across raw JSON. */
        r = json_variant_format(contents, 0, &contents_str);
        if (r < 0)
                return r;
        r = sd_bus_message_append(reply, "{sv}", "_contents", "s", contents_str);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int target_method_describe(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Target *t = ASSERT_PTR(userdata);
        _cleanup_(job_freep) Job *j = NULL;
        const char *version;
        bool offline;
        int r;

        assert(msg);

        r = sd_bus_message_read(msg, "sb", &version, &offline);
        if (r < 0)
                return r;

        if (isempty(version))
                return -EINVAL;

        const char *details[] = {
                "class", target_class_to_string(t->class),
                "name", t->name,
                "version", version,
                "offline", true_false(offline),
                NULL
        };

        r = bus_verify_polkit_async(
                msg,
                CAP_SYS_ADMIN,
                "org.freedesktop.sysupdate1.check",
                details,
                /* interactive= */ false,
                UID_INVALID,
                &t->manager->polkit_registry,
                error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_new(JOB_DESCRIBE, t, msg, target_method_describe_finish, &j);
        if (r < 0)
                return r;

        j->version = strdup(version);
        if (!j->version)
                return log_oom();

        j->offline = offline;

        r = job_start(j);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to start job: %m");
        TAKE_PTR(j); /* Avoid job from being killed & freed */

        return 1;
}

static int target_method_check_new_finish(
                sd_bus_message *msg,
                const Job *j,
                JsonVariant *json,
                sd_bus_error *error) {
        const char *reply;

        assert(json);

        if (json_variant_is_null(json))
                reply = "";
        else
                reply = json_variant_string(json);
        if (!reply)
                return -EINVAL;

        return sd_bus_reply_method_return(msg, "s", reply);
}

static int target_method_check_new(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Target *t = ASSERT_PTR(userdata);
        _cleanup_(job_freep) Job *j = NULL;
        int r;

        assert(msg);

        const char *details[] = {
                "class", target_class_to_string(t->class),
                "name", t->name,
                "offline", "false",
                NULL
        };

        r = bus_verify_polkit_async(
                msg,
                CAP_SYS_ADMIN,
                "org.freedesktop.sysupdate1.check",
                details,
                /* interactive= */ false,
                UID_INVALID,
                &t->manager->polkit_registry,
                error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_new(JOB_CHECK_NEW, t, msg, target_method_check_new_finish, &j);
        if (r < 0)
                return r;

        r = job_start(j);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to start job: %m");
        TAKE_PTR(j); /* Avoid job from being killed & freed */

        return 1;
}

static int target_method_update_finished_early(
                sd_bus_message *msg,
                const Job *j,
                JsonVariant *json,
                sd_bus_error *error) {

        /* Called when job finishes w/ a successful exit code, but before any work begins.
         * This happens when there is no candidate (i.e. we're already up-to-date), or
         * specified update is already installed. */
        return sd_bus_error_set_errno(error, -EALREADY);
}

static int target_method_update(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Target *t = ASSERT_PTR(userdata);
        _cleanup_(job_freep) Job *j = NULL;
        const char *version, *action;
        int r;

        assert(msg);

        r = sd_bus_message_read(msg, "s", &version);
        if (r < 0)
                return r;

        if (isempty(version))
                action = "org.freedesktop.sysupdate1.update";
        else
                action = "org.freedesktop.sysupdate1.update-to-version";

        const char *details[] = {
                "class", target_class_to_string(t->class),
                "name", t->name,
                "version", version,
                NULL
        };

        r = bus_verify_polkit_async(
                msg,
                CAP_SYS_ADMIN,
                action,
                details,
                /* interactive= */ false,
                UID_INVALID,
                &t->manager->polkit_registry,
                error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_new(JOB_UPDATE, t, msg, target_method_update_finished_early, &j);
        if (r < 0)
                return r;

        j->detach_on_ready = true;

        j->version = strdup(version);
        if (!j->version)
                return log_oom();

        r = job_start(j);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to start job: %m");
        TAKE_PTR(j);

        return 1;
}

static int target_method_vacuum_finish(
                sd_bus_message *msg,
                const Job *j,
                JsonVariant *json,
                sd_bus_error *error) {

        assert(json);
        return sd_bus_reply_method_return(msg, "u", json_variant_unsigned(json));
}

static int target_method_vacuum(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        Target *t = ASSERT_PTR(userdata);
        _cleanup_(job_freep) Job *j = NULL;
        int r;

        assert(msg);

        const char *details[] = {
                "class", target_class_to_string(t->class),
                "name", t->name,
                NULL
        };

        r = bus_verify_polkit_async(
                msg,
                CAP_SYS_ADMIN,
                "org.freedesktop.sysupdate1.vacuum",
                details,
                /* interactive= */ false,
                UID_INVALID,
                &t->manager->polkit_registry,
                error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Will call us back */

        r = job_new(JOB_VACUUM, t, msg, target_method_vacuum_finish, &j);
        if (r < 0)
                return r;

        r = job_start(j);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to start job: %m");
        TAKE_PTR(j); /* Avoid job from being killed & freed */

        return 1;
}

static int target_list_components(Target *t, char ***ret_components, bool *ret_have_default) {
        _cleanup_(json_variant_unrefp) JsonVariant *json = NULL;
        _cleanup_strv_free_ char **components = NULL;
        _cleanup_free_ char *target_arg = NULL;
        JsonVariant *v;
        bool have_default;
        int r;

        if (t) {
                r = target_get_argument(t, &target_arg);
                if (r < 0)
                        return r;
        }

        r = sysupdate_run_simple(&json, "components", target_arg, NULL);
        if (r < 0)
                return r;

        v = json_variant_by_key(json, "default");
        if (!v)
                return -EINVAL;
        have_default = json_variant_boolean(v);

        v = json_variant_by_key(json, "components");
        if (!v)
                return -EINVAL;
        r = json_variant_strv(v, &components);
        if (r < 0)
                return r;

        if (ret_components)
                *ret_components = TAKE_PTR(components);
        if (ret_have_default)
                *ret_have_default = have_default;
        return 0;
}

static int manager_ensure_targets(Manager *m);

static int target_object_find(
                sd_bus *bus,
                const char *path,
                const char *iface,
                void *userdata,
                void **found,
                sd_bus_error *error) {

        Manager *m = ASSERT_PTR(userdata);
        Target *t;
        _cleanup_free_ char *e = NULL;
        const char *p;
        int r;

        assert(bus);
        assert(path);
        assert(found);

        p = startswith(path, "/org/freedesktop/sysupdate1/target/");
        if (!p)
                return 0;

        e = bus_label_unescape(p);
        if (!e)
                return -ENOMEM;

        r = manager_ensure_targets(m);
        if (r < 0)
                return r;

        t = hashmap_get(m->targets, e);
        if (!t)
                return 0;

        *found = t;
        return 1;
}

static char *target_bus_path(Target *t) {
        _cleanup_free_ char *e = NULL;

        assert(t);

        e = bus_label_escape(t->id);
        if (!e)
                return NULL;

        return strjoin("/org/freedesktop/sysupdate1/target/", e);
}

static int target_node_enumerator(
                sd_bus *bus,
                const char *path,
                void *userdata,
                char ***nodes,
                sd_bus_error *error) {

        _cleanup_strv_free_ char **l = NULL;
        Manager *m = ASSERT_PTR(userdata);
        Target *t;
        unsigned k = 0;
        int r;

        r = manager_ensure_targets(m);
        if (r < 0)
                return r;

        l = new0(char*, hashmap_size(m->targets) + 1);
        if (!l)
                return -ENOMEM;

        HASHMAP_FOREACH(t, m->targets) {
                l[k] = target_bus_path(t);
                if (!l[k])
                        return -ENOMEM;
                k++;
        }

        *nodes = TAKE_PTR(l);
        return 1;
}

static const sd_bus_vtable target_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Class", "s", target_property_get_class,
                        offsetof(Target, class), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Name", "s", NULL, offsetof(Target, name),
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Path", "s", NULL, offsetof(Target, path),
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Version", "s", target_property_get_cached, 0, 0),
        SD_BUS_PROPERTY("Appstream", "s", target_property_get_cached, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),

        SD_BUS_METHOD_WITH_ARGS("List",
                                SD_BUS_ARGS("b", offline),
                                SD_BUS_RESULT("as", versions),
                                target_method_list,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("Describe",
                                SD_BUS_ARGS("s", version, "b", offline),
                                SD_BUS_RESULT("a{sv}", properties),
                                target_method_describe,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("CheckNew",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("s", new_version),
                                target_method_check_new,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("Update",
                                SD_BUS_ARGS("s", new_version),
                                SD_BUS_RESULT("s", new_version, "u", job_id, "o", job_path),
                                target_method_update,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("Vacuum",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("u", count),
                                target_method_vacuum,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_VTABLE_END
};

static const BusObjectImplementation target_object = {
        "/org/freedesktop/sysupdate1/target",
        "org.freedesktop.sysupdate1.Target",
        .fallback_vtables = BUS_FALLBACK_VTABLES({target_vtable, target_object_find}),
        .node_enumerator = target_node_enumerator,
};

static Manager *manager_free(Manager *m) {
        if (!m)
                return NULL;

        hashmap_free(m->targets);
        hashmap_free(m->jobs);

        bus_verify_polkit_async_registry_free(m->polkit_registry);

        m->bus = sd_bus_flush_close_unref(m->bus);
        sd_event_source_unref(m->notify_event);
        sd_event_unref(m->event);

        return mfree(m);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Manager *, manager_free);

static int manager_on_notify(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        char buf[NOTIFY_BUFFER_MAX+1];
        struct iovec iovec = {
                .iov_base = buf,
                .iov_len = sizeof(buf)-1,
        };
        CMSG_BUFFER_TYPE(CMSG_SPACE(sizeof(struct ucred))) control;
        struct msghdr msghdr = {
                .msg_iov = &iovec,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
        };
        struct ucred *ucred;
        Manager *m = ASSERT_PTR(userdata);
        Job *j;
        ssize_t n;
        char *p;

        n = recvmsg_safe(fd, &msghdr, MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if (n < 0) {
                if (ERRNO_IS_TRANSIENT(n))
                        return 0;
                return (int) n;
        }

        cmsg_close_all(&msghdr);

        if (msghdr.msg_flags & MSG_TRUNC) {
                log_warning("Got overly long notification datagram, ignoring.");
                return 0;
        }

        ucred = CMSG_FIND_DATA(&msghdr, SOL_SOCKET, SCM_CREDENTIALS, struct ucred);
        if (!ucred || ucred->pid <= 0) {
                log_warning("Got notification datagram lacking credential information, ignoring.");
                return 0;
        }

        HASHMAP_FOREACH(j, m->jobs) {
                pid_t pid;
                assert_se(sd_event_source_get_child_pid(j->child, &pid) >= 0);

                if (ucred->pid == pid)
                        break;
        }

        if (!j) {
                log_warning("Got notification datagram from unexpected peer, ignoring.");
                return 0;
        }

        buf[n] = 0;

        p = find_line_startswith(buf, "X_UPDATE_VERSION=");
        if (p)
                job_on_version(j, strdupcspn(p, "\n"));

        p = find_line_startswith(buf, "ERRNO=");
        if (p)
                job_on_errno(j, strdupcspn(p, "\n"));

        p = find_line_startswith(buf, "X_UPDATE_PROGRESS=");
        if (p)
                job_on_progress(j, strdupcspn(p, "\n"));

        /* Should come last, since this might actually detach the job */
        if (find_line_startswith(buf, "READY=1"))
                job_on_ready(j);

        return 0;
}

static int manager_new(Manager **ret) {
        _cleanup_(manager_freep) Manager *m = NULL;
        _cleanup_close_ int notify_fd = -EBADF;
        static const union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
                .un.sun_path = "/run/systemd/sysupdate/notify",
        };
        int r;

        assert(ret);

        m = new(Manager, 1);
        if (!m)
                return -ENOMEM;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        (void) sd_event_set_watchdog(m->event, true);

        r = sd_event_set_signal_exit(m->event, true);
        if (r < 0)
                return r;

        r = sd_event_add_signal(m->event, NULL, (SIGRTMIN+18) | SD_EVENT_SIGNAL_PROCMASK,
                                sigrtmin18_handler, NULL);
        if (r < 0)
                return r;

        r = sd_event_add_memory_pressure(m->event, NULL, NULL, NULL);
        if (r < 0)
                log_debug_errno(r, "Failed allocate memory pressure event source, ignoring: %m");

        r = sd_bus_default_system(&m->bus);
        if (r < 0)
                return r;

        notify_fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (notify_fd < 0)
                return -errno;

        (void) mkdir_parents_label(sa.un.sun_path, 0755);
        (void) sockaddr_un_unlink(&sa.un);

        if (bind(notify_fd, &sa.sa, SOCKADDR_UN_LEN(sa.un)) < 0)
                return -errno;

        r = setsockopt_int(notify_fd, SOL_SOCKET, SO_PASSCRED, true);
        if (r < 0)
                return r;

        r = sd_event_add_io(m->event, &m->notify_event, notify_fd, EPOLLIN, manager_on_notify, m);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(m->notify_event, "notify-socket");

        r = sd_event_source_set_io_fd_own(m->notify_event, true);
        if (r < 0)
                return r;
        TAKE_FD(notify_fd);

        *ret = TAKE_PTR(m);
        return 0;
}

static int manager_enumerate_image_class(Manager *m, TargetClass class) {
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        Image *image;
        int r;

        images = hashmap_new(&image_hash_ops);
        if (!images)
                return log_oom();

        r = image_discover((ImageClass) class, NULL, images);
        if (r < 0)
                return r;

        HASHMAP_FOREACH(image, images) {
                _cleanup_(target_freep) Target *t = NULL;
                bool have = false;

                if (IMAGE_IS_HOST(image))
                        continue; /* We already enroll the host ourselves */

                r = target_new(m, class, image->name, image->path, &t);
                if (r < 0)
                        return r;
                t->image_type = image->type;

                r = target_list_components(t, NULL, &have);
                if (r < 0)
                        return r;
                if (!have) {
                        log_debug("Skipping %s because it has no default component", image->path);
                        continue;
                }
                TAKE_PTR(t); /* Now owned by m->targets */
        }

        return 0;
}

static int manager_enumerate_components(Manager *m) {
        _cleanup_strv_free_ char **components = NULL;
        bool have_default;
        Target *t;
        int r;

        r = target_list_components(NULL, &components, &have_default);
        if (r < 0)
                return r;

        if (have_default) {
                r = target_new(m, TARGET_HOST, ".host", "sysupdate.d", &t);
                if (r < 0)
                        return r;
        }

        STRV_FOREACH(component, components) {
                _cleanup_free_ char *path = NULL;

                path = strjoin("sysupdate.", *component, ".d");
                if (!path)
                        return log_oom();

                r = target_new(m, TARGET_COMPONENT, *component, path, &t);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int manager_enumerate_targets(Manager *m) {
        static const TargetClass discoverable_classes[] = {
                TARGET_MACHINE,
                TARGET_PORTABLE,
                TARGET_SYSEXT,
                TARGET_CONFEXT,
        };
        int r;

        assert(m);

        FOREACH_ARRAY(class, discoverable_classes, ELEMENTSOF(discoverable_classes)) {
                r = manager_enumerate_image_class(m, *class);
                if (r < 0)
                        log_warning_errno(r, "Failed to enumerate %ss, ignoring: %m",
                                          target_class_to_string(*class));
        }

        return manager_enumerate_components(m);
}

static int manager_ensure_targets(Manager *m) {
        assert(m);

        if (!hashmap_isempty(m->targets))
                return 0;

        return manager_enumerate_targets(m);
}

static int method_list_targets(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        Manager *m = ASSERT_PTR(userdata);
        Target *t;
        int r;

        assert(msg);

        r = manager_ensure_targets(m);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(msg, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(sso)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(t, m->targets) {
                _cleanup_free_ char *bus_path = NULL;

                bus_path = target_bus_path(t);
                if (!bus_path)
                        return log_oom();

                r = sd_bus_message_append(reply, "(sso)",
                                          target_class_to_string(t->class),
                                          t->name,
                                          bus_path);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int method_list_jobs(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        Manager *m = ASSERT_PTR(userdata);
        Job *j;
        int r;

        assert(msg);

        r = sd_bus_message_new_method_return(msg, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(usuo)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(j, m->jobs) {
                r = sd_bus_message_append(reply, "(usuo)",
                                          j->id,
                                          job_type_to_string(j->type),
                                          j->progress_percent,
                                          j->object_path);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int method_list_appstream(sd_bus_message *msg, void *userdata, sd_bus_error *error) {
        _cleanup_strv_free_ char **urls = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        Manager *m = ASSERT_PTR(userdata);
        Target *t;
        int r;

        assert(msg);

        r = manager_ensure_targets(m);
        if (r < 0)
                return r;

        HASHMAP_FOREACH(t, m->targets) {
                r = target_ensure_properties(t);
                if (r < 0)
                        return r;

                if (isempty(t->appstream))
                        continue;

                r = strv_extend(&urls, t->appstream);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_new_method_return(msg, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, urls);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static const sd_bus_vtable manager_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_METHOD_WITH_ARGS("ListTargets",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("a(sso)", targets),
                                method_list_targets,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("ListJobs",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("a(usuo)", jobs),
                                method_list_jobs,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_METHOD_WITH_ARGS("ListAppstream",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("as", urls),
                                method_list_appstream,
                                SD_BUS_VTABLE_UNPRIVILEGED),

        SD_BUS_SIGNAL_WITH_ARGS("JobRemoved",
                                SD_BUS_ARGS("u", id, "o", path, "i", status),
                                0),

        SD_BUS_VTABLE_END
};

static const BusObjectImplementation manager_object = {
        "/org/freedesktop/sysupdate1",
        "org.freedesktop.sysupdate1.Manager",
        .vtables = BUS_VTABLES(manager_vtable),
        .children = BUS_IMPLEMENTATIONS(&job_object, &target_object),
};

static int manager_add_bus_objects(Manager *m) {
        int r;

        assert(m);

        r = bus_add_implementation(m->bus, &manager_object, m);
        if (r < 0)
                return r;

        r = bus_log_control_api_register(m->bus);
        if (r < 0)
                return r;

        r = sd_bus_request_name_async(m->bus, NULL, "org.freedesktop.sysupdate1", 0, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to request name: %m");

        r = sd_bus_attach_event(m->bus, m->event, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to attach bus to event loop: %m");

        return 0;
}

static bool manager_idle(void *userdata) {
        Manager *m = ASSERT_PTR(userdata);

        if (!hashmap_isempty(m->jobs))
                return false;

        /* The daemon is completely idle, and we will exit in ~30s as long as
         * no new clients appear. This is the only time we can safely clear
         * out our targets, so that when a new client appears it will see
         * up-to-date information.
         */
        hashmap_clear(m->targets);
        log_debug("Cleared target cache");

        return true;
}

static int manager_run(Manager *m) {
        assert(m);

        return bus_event_loop_with_idle(m->event,
                                        m->bus,
                                        "org.freedesktop.sysupdate1",
                                        DEFAULT_EXIT_USEC,
                                        manager_idle,
                                        m);
}

static int run(int argc, char *argv[]) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        log_setup();

        r = service_parse_argv("systemd-sysupdated.service",
                               "System update management service.",
                               BUS_IMPLEMENTATIONS(&manager_object,
                                                   &log_control_object),
                               argc, argv);
        if (r <= 0)
                return r;

        umask(0022);

        r = sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, -1);
        if (r < 0)
                return log_error_errno(r, "Failed to mask SIGCHILD: %m");

        r = manager_new(&m);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate manager object: %m");

        r = manager_add_bus_objects(m);
        if (r < 0)
                return r;

        r = manager_run(m);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
