/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-id128.h"

#include "alloc-util.h"
#include "blockdev-util.h"
#include "build-path.h"
#include "chase.h"
#include "conf-parser.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "glyph-util.h"
#include "gpt.h"
#include "hexdecoct.h"
#include "install-file.h"
#include "mkdir.h"
#include "parse-helpers.h"
#include "parse-util.h"
#include "process-util.h"
#include "random-util.h"
#include "rm-rf.h"
#include "signal-util.h"
#include "socket-util.h"
#include "specifier.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "strv.h"
#include "sync-util.h"
#include "sysupdate-pattern.h"
#include "sysupdate-resource.h"
#include "sysupdate-transfer.h"
#include "sysupdate.h"
#include "tmpfile-util.h"
#include "web-util.h"

/* Default value for InstancesMax= for fs object targets */
#define DEFAULT_FILE_INSTANCES_MAX 3

Transfer *transfer_free(Transfer *t) {
        if (!t)
                return NULL;

        t->temporary_path = rm_rf_subvolume_and_free(t->temporary_path);

        free(t->definition_path);
        free(t->min_version);
        strv_free(t->protected_versions);
        free(t->current_symlink);
        free(t->final_path);

        free(t->changelog);
        free(t->appstream);

        partition_info_destroy(&t->partition_info);

        resource_destroy(&t->source);
        resource_destroy(&t->target);

        return mfree(t);
}

Transfer *transfer_new(void) {
        Transfer *t;

        t = new(Transfer, 1);
        if (!t)
                return NULL;

        *t = (Transfer) {
                .source.type = _RESOURCE_TYPE_INVALID,
                .target.type = _RESOURCE_TYPE_INVALID,
                .remove_temporary = true,
                .mode = MODE_INVALID,
                .tries_left = UINT64_MAX,
                .tries_done = UINT64_MAX,
                .verify = true,

                /* the three flags, as configured by the user */
                .no_auto = -1,
                .read_only = -1,
                .growfs = -1,

                /* the read only flag, as ultimately determined */
                .install_read_only = -1,

                .partition_info = PARTITION_INFO_NULL,
        };

        return t;
}

static const Specifier specifier_table[] = {
        COMMON_SYSTEM_SPECIFIERS,
        COMMON_TMP_SPECIFIERS,
        {}
};

static int config_parse_protect_version(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_free_ char *resolved = NULL;
        char ***protected_versions = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = specifier_printf(rvalue, NAME_MAX, specifier_table, arg_root, NULL, &resolved);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to expand specifiers in ProtectVersion=, ignoring: %s", rvalue);
                return 0;
        }

        if (!version_is_valid(resolved))  {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "ProtectVersion= string is not valid, ignoring: %s", resolved);
                return 0;
        }

        r = strv_extend(protected_versions, resolved);
        if (r < 0)
                return log_oom();

        return 0;
}

static int config_parse_min_version(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_free_ char *resolved = NULL;
        char **version = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = specifier_printf(rvalue, NAME_MAX, specifier_table, arg_root, NULL, &resolved);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to expand specifiers in MinVersion=, ignoring: %s", rvalue);
                return 0;
        }

        if (!version_is_valid(rvalue)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "MinVersion= string is not valid, ignoring: %s", resolved);
                return 0;
        }

        return free_and_replace(*version, resolved);
}

static int config_parse_url_specifiers(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        char **s = ASSERT_PTR(data);
        _cleanup_free_ char *resolved = NULL;
        int r;

        assert(rvalue);

        if (isempty(rvalue)) {
                *s = mfree(*s);
                return 0;
        }

        r = specifier_printf(rvalue, NAME_MAX, specifier_table, arg_root, NULL, &resolved);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to expand specifiers in %s=, ignoring: %s", lvalue, rvalue);
                return 0;
        }

        if (!http_url_is_valid(resolved)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "%s= URL is not valid, ignoring: %s", lvalue, rvalue);
                return 0;
        }

        return free_and_replace(*s, resolved);
}

static int config_parse_current_symlink(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_free_ char *resolved = NULL;
        char **current_symlink = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = specifier_printf(rvalue, NAME_MAX, specifier_table, arg_root, NULL, &resolved);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to expand specifiers in CurrentSymlink=, ignoring: %s", rvalue);
                return 0;
        }

        r = path_simplify_and_warn(resolved, 0, unit, filename, line, lvalue);
        if (r < 0)
                return 0;

        return free_and_replace(*current_symlink, resolved);
}

static int config_parse_instances_max(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint64_t *instances_max = data, i;
        int r;

        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                *instances_max = 0; /* Revert to default logic, see transfer_read_definition() */
                return 0;
        }

        r = safe_atou64(rvalue, &i);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse InstancesMax= value, ignoring: %s", rvalue);
                return 0;
        }

        if (i < 2) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "InstancesMax= value must be at least 2, bumping: %s", rvalue);
                *instances_max = 2;
        } else
                *instances_max = i;

        return 0;
}

static int config_parse_resource_pattern(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        char ***patterns = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        if (isempty(rvalue)) {
                *patterns = strv_free(*patterns);
                return 0;
        }

        for (;;) {
                _cleanup_free_ char *word = NULL, *resolved = NULL;

                r = extract_first_word(&rvalue, &word, NULL, EXTRACT_CUNESCAPE|EXTRACT_UNESCAPE_RELAX);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to extract first pattern from MatchPattern=, ignoring: %s", rvalue);
                        return 0;
                }
                if (r == 0)
                        break;

                r = specifier_printf(word, NAME_MAX, specifier_table, arg_root, NULL, &resolved);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to expand specifiers in MatchPattern=, ignoring: %s", rvalue);
                        return 0;
                }

                if (!pattern_valid(resolved))
                        return log_syntax(unit, LOG_ERR, filename, line, SYNTHETIC_ERRNO(EINVAL),
                                          "MatchPattern= string is not valid, refusing: %s", resolved);

                r = strv_consume(patterns, TAKE_PTR(resolved));
                if (r < 0)
                        return log_oom();
        }

        strv_uniq(*patterns);
        return 0;
}

static int config_parse_resource_path(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        _cleanup_free_ char *resolved = NULL;
        Resource *rr = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        if (streq(rvalue, "auto")) {
                rr->path_auto = true;
                rr->path = mfree(rr->path);
                return 0;
        }

        r = specifier_printf(rvalue, PATH_MAX-1, specifier_table, arg_root, NULL, &resolved);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to expand specifiers in Path=, ignoring: %s", rvalue);
                return 0;
        }

        /* Note that we don't validate the path as being absolute or normalized. We'll do that in
         * transfer_read_definition() as we might not know yet whether Path refers to a URL or a file system
         * path. */

        rr->path_auto = false;
        return free_and_replace(rr->path, resolved);
}

static DEFINE_CONFIG_PARSE_ENUM(config_parse_resource_type, resource_type, ResourceType, "Invalid resource type");

static DEFINE_CONFIG_PARSE_ENUM_WITH_DEFAULT(config_parse_resource_path_relto, path_relative_to, PathRelativeTo,
                                             PATH_RELATIVE_TO_ROOT, "Invalid PathRelativeTo= value");

static int config_parse_resource_ptype(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Resource *rr = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = gpt_partition_type_from_string(rvalue, &rr->partition_type);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed parse partition type, ignoring: %s", rvalue);
                return 0;
        }

        rr->partition_type_set = true;
        return 0;
}

static int config_parse_partition_uuid(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Transfer *t = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = sd_id128_from_string(rvalue, &t->partition_uuid);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed parse partition UUID, ignoring: %s", rvalue);
                return 0;
        }

        t->partition_uuid_set = true;
        return 0;
}

static int config_parse_partition_flags(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Transfer *t = ASSERT_PTR(data);
        int r;

        assert(rvalue);

        r = safe_atou64(rvalue, &t->partition_flags);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed parse partition flags, ignoring: %s", rvalue);
                return 0;
        }

        t->partition_flags_set = true;
        return 0;
}

int transfer_read_definition(Transfer *t, const char *path) {
        int r;

        assert(t);
        assert(path);

        ConfigTableItem table[] = {
                { "Transfer",    "MinVersion",              config_parse_min_version,          0, &t->min_version             },
                { "Transfer",    "ProtectVersion",          config_parse_protect_version,      0, &t->protected_versions      },
                { "Transfer",    "Verify",                  config_parse_bool,                 0, &t->verify                  },
                { "Transfer",    "Changelog",               config_parse_url_specifiers,       0, &t->changelog               },
                { "Transfer",    "Appstream",               config_parse_url_specifiers,       0, &t->appstream               },
                { "Source",      "Type",                    config_parse_resource_type,        0, &t->source.type             },
                { "Source",      "Path",                    config_parse_resource_path,        0, &t->source                  },
                { "Source",      "PathRelativeTo",          config_parse_resource_path_relto,  0, &t->source.path_relative_to },
                { "Source",      "MatchPattern",            config_parse_resource_pattern,     0, &t->source.patterns         },
                { "Target",      "Type",                    config_parse_resource_type,        0, &t->target.type             },
                { "Target",      "Path",                    config_parse_resource_path,        0, &t->target                  },
                { "Target",      "PathRelativeTo",          config_parse_resource_path_relto,  0, &t->target.path_relative_to },
                { "Target",      "MatchPattern",            config_parse_resource_pattern,     0, &t->target.patterns         },
                { "Target",      "MatchPartitionType",      config_parse_resource_ptype,       0, &t->target                  },
                { "Target",      "PartitionUUID",           config_parse_partition_uuid,       0, t                           },
                { "Target",      "PartitionFlags",          config_parse_partition_flags,      0, t                           },
                { "Target",      "PartitionNoAuto",         config_parse_tristate,             0, &t->no_auto                 },
                { "Target",      "PartitionGrowFileSystem", config_parse_tristate,             0, &t->growfs                  },
                { "Target",      "ReadOnly",                config_parse_tristate,             0, &t->read_only               },
                { "Target",      "Mode",                    config_parse_mode,                 0, &t->mode                    },
                { "Target",      "TriesLeft",               config_parse_uint64,               0, &t->tries_left              },
                { "Target",      "TriesDone",               config_parse_uint64,               0, &t->tries_done              },
                { "Target",      "InstancesMax",            config_parse_instances_max,        0, &t->instances_max           },
                { "Target",      "RemoveTemporary",         config_parse_bool,                 0, &t->remove_temporary        },
                { "Target",      "CurrentSymlink",          config_parse_current_symlink,      0, &t->current_symlink         },
                {}
        };

        r = config_parse(NULL, path, NULL,
                         "Transfer\0"
                         "Source\0"
                         "Target\0",
                         config_item_table_lookup, table,
                         CONFIG_PARSE_WARN,
                         t,
                         NULL);
        if (r < 0)
                return r;

        if (!RESOURCE_IS_SOURCE(t->source.type))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Source Type= must be one of url-file, url-tar, tar, regular-file, directory, subvolume.");

        if (t->target.type < 0) {
                switch (t->source.type) {

                case RESOURCE_URL_FILE:
                case RESOURCE_REGULAR_FILE:
                        t->target.type =
                                t->target.path && path_startswith(t->target.path, "/dev/") ?
                                RESOURCE_PARTITION : RESOURCE_REGULAR_FILE;
                        break;

                case RESOURCE_URL_TAR:
                case RESOURCE_TAR:
                case RESOURCE_DIRECTORY:
                        t->target.type = RESOURCE_DIRECTORY;
                        break;

                case RESOURCE_SUBVOLUME:
                        t->target.type = RESOURCE_SUBVOLUME;
                        break;

                default:
                        assert_not_reached();
                }
        }

        if (!RESOURCE_IS_TARGET(t->target.type))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Target Type= must be one of partition, regular-file, directory, subvolume.");

        if ((IN_SET(t->source.type, RESOURCE_URL_FILE, RESOURCE_PARTITION, RESOURCE_REGULAR_FILE) &&
             !IN_SET(t->target.type, RESOURCE_PARTITION, RESOURCE_REGULAR_FILE)) ||
            (IN_SET(t->source.type, RESOURCE_URL_TAR, RESOURCE_TAR, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME) &&
             !IN_SET(t->target.type, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME)))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Target type '%s' is incompatible with source type '%s', refusing.",
                                  resource_type_to_string(t->source.type), resource_type_to_string(t->target.type));

        if (!t->source.path && !t->source.path_auto)
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Source specification lacks Path=.");

        if (t->source.path) {
                if (RESOURCE_IS_FILESYSTEM(t->source.type) || t->source.type == RESOURCE_PARTITION)
                        if (!path_is_absolute(t->source.path) || !path_is_normalized(t->source.path))
                                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                                  "Source path is not a normalized, absolute path: %s", t->source.path);

                /* We unofficially support file:// in addition to http:// and https:// for url
                 * sources. That's mostly for testing, since it relieves us from having to set up a HTTP
                 * server, and CURL abstracts this away from us thankfully. */
                if (RESOURCE_IS_URL(t->source.type))
                        if (!http_url_is_valid(t->source.path) && !file_url_is_valid(t->source.path))
                                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                                  "Source path is not a valid HTTP or HTTPS URL: %s", t->source.path);
        }

        if (strv_isempty(t->source.patterns))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Source specification lacks MatchPattern=.");

        if (!t->target.path && !t->target.path_auto)
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Target specification lacks Path= field.");

        if (t->target.path &&
            (!path_is_absolute(t->target.path) || !path_is_normalized(t->target.path)))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Target path is not a normalized, absolute path: %s", t->target.path);

        if (strv_isempty(t->target.patterns)) {
                strv_free(t->target.patterns);
                t->target.patterns = strv_copy(t->source.patterns);
                if (!t->target.patterns)
                        return log_oom();
        }

        if (t->current_symlink && !RESOURCE_IS_FILESYSTEM(t->target.type) && !path_is_absolute(t->current_symlink))
                return log_syntax(NULL, LOG_ERR, path, 1, SYNTHETIC_ERRNO(EINVAL),
                                  "Current symlink must be absolute path if target is partition: %s", t->current_symlink);

        /* When no instance limit is set, use all available partition slots in case of partitions, or 3 in case of fs objects */
        if (t->instances_max == 0)
                t->instances_max = t->target.type == RESOURCE_PARTITION ? UINT64_MAX : DEFAULT_FILE_INSTANCES_MAX;

        return 0;
}

int transfer_resolve_paths(
                Transfer *t,
                const char *root,
                const char *node) {

        int r;

        /* If Path=auto is used in [Source] or [Target] sections, let's automatically detect the path of the
         * block device to use. Moreover, if this path points to a directory but we need a block device,
         * automatically determine the backing block device, so that users can reference block devices by
         * mount point. */

        assert(t);

        r = resource_resolve_path(&t->source, root, node);
        if (r < 0)
                return r;

        r = resource_resolve_path(&t->target, root, node);
        if (r < 0)
                return r;

        return 0;
}

static void transfer_remove_temporary(Transfer *t) {
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        assert(t);

        if (!t->remove_temporary)
                return;

        if (!IN_SET(t->target.type, RESOURCE_REGULAR_FILE, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME))
                return;

        /* Removes all temporary files/dirs from previous runs in the target directory, i.e. all those starting with '.#' */

        d = opendir(t->target.path);
        if (!d) {
                if (errno == ENOENT)
                        return;

                log_debug_errno(errno, "Failed to open target directory '%s', ignoring: %m", t->target.path);
                return;
        }

        for (;;) {
                struct dirent *de;

                errno = 0;
                de = readdir_no_dot(d);
                if (!de) {
                        if (errno != 0)
                                log_debug_errno(errno, "Failed to read target directory '%s', ignoring: %m", t->target.path);
                        break;
                }

                if (!startswith(de->d_name, ".#"))
                        continue;

                r = rm_rf_child(dirfd(d), de->d_name, REMOVE_PHYSICAL|REMOVE_SUBVOLUME|REMOVE_CHMOD);
                if (r == -ENOENT)
                        continue;
                if (r < 0) {
                        log_warning_errno(r, "Failed to remove temporary resource instance '%s/%s', ignoring: %m", t->target.path, de->d_name);
                        continue;
                }

                log_debug("Removed temporary resource instance '%s/%s'.", t->target.path, de->d_name);
        }
}

int transfer_vacuum(
                Transfer *t,
                uint64_t space,
                const char *extra_protected_version) {

        uint64_t instances_max, limit;
        int r, count = 0;

        assert(t);

        transfer_remove_temporary(t);

        /* First, calculate how many instances to keep, based on the instance limit — but keep at least one */

        instances_max = arg_instances_max != UINT64_MAX ? arg_instances_max : t->instances_max;
        assert(instances_max >= 1);
        if (instances_max == UINT64_MAX) /* Keep infinite instances? */
                limit = UINT64_MAX;
        else if (space > instances_max)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSPC),
                                       "Asked to delete more instances than total maximum allowed number of instances, refusing.");
        else if (space == instances_max)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSPC),
                                       "Asked to delete all possible instances, can't allow that. One instance must always remain.");
        else
                limit = instances_max - space;

        if (t->target.type == RESOURCE_PARTITION) {
                uint64_t rm, remain;

                /* If we are looking at a partition table, we also have to take into account how many
                 * partition slots of the right type are available */

                if (t->target.n_empty + t->target.n_instances < 2)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOSPC),
                                               "Partition table has less than two partition slots of the right type " SD_ID128_UUID_FORMAT_STR " (%s), refusing.",
                                               SD_ID128_FORMAT_VAL(t->target.partition_type.uuid),
                                               gpt_partition_type_uuid_to_string(t->target.partition_type.uuid));
                if (space > t->target.n_empty + t->target.n_instances)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOSPC),
                                               "Partition table does not have enough partition slots of right type " SD_ID128_UUID_FORMAT_STR " (%s) for operation.",
                                               SD_ID128_FORMAT_VAL(t->target.partition_type.uuid),
                                               gpt_partition_type_uuid_to_string(t->target.partition_type.uuid));
                if (space == t->target.n_empty + t->target.n_instances)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOSPC),
                                               "Asked to empty all partition table slots of the right type " SD_ID128_UUID_FORMAT_STR " (%s), can't allow that. One instance must always remain.",
                                               SD_ID128_FORMAT_VAL(t->target.partition_type.uuid),
                                               gpt_partition_type_uuid_to_string(t->target.partition_type.uuid));

                rm = LESS_BY(space, t->target.n_empty);
                remain = LESS_BY(t->target.n_instances, rm);
                limit = MIN(limit, remain);
        }

        while (t->target.n_instances > limit) {
                Instance *oldest;
                size_t p = t->target.n_instances - 1;

                for (;;) {
                        oldest = t->target.instances[p];
                        assert(oldest);

                        /* If this is listed among the protected versions, then let's not remove it */
                        if (!strv_contains(t->protected_versions, oldest->metadata.version) &&
                            (!extra_protected_version || !streq(extra_protected_version, oldest->metadata.version)))
                                break;

                        log_debug("Version '%s' is protected, not removing.", oldest->metadata.version);
                        if (p == 0) {
                                oldest = NULL;
                                break;
                        }

                        p--;
                }

                if (!oldest) /* Nothing more to remove */
                        break;

                assert(oldest->resource);

                log_info("%s Removing old '%s' (%s).", special_glyph(SPECIAL_GLYPH_RECYCLING), oldest->path, resource_type_to_string(oldest->resource->type));

                switch (t->target.type) {

                case RESOURCE_REGULAR_FILE:
                case RESOURCE_DIRECTORY:
                case RESOURCE_SUBVOLUME:
                        r = rm_rf(oldest->path, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_SUBVOLUME|REMOVE_MISSING_OK|REMOVE_CHMOD);
                        if (r < 0 && r != -ENOENT)
                                return log_error_errno(r, "Failed to make room, deleting '%s' failed: %m", oldest->path);

                        (void) rmdir_parents(oldest->path, t->target.path);

                        break;

                case RESOURCE_PARTITION: {
                        PartitionInfo pinfo = oldest->partition_info;

                        /* label "_empty" means "no contents" for our purposes */
                        pinfo.label = (char*) "_empty";

                        r = patch_partition(t->target.path, &pinfo, PARTITION_LABEL);
                        if (r < 0)
                                return r;

                        t->target.n_empty++;
                        break;
                }

                default:
                        assert_not_reached();
                        break;
                }

                instance_free(oldest);
                memmove(t->target.instances + p, t->target.instances + p + 1, (t->target.n_instances - p - 1) * sizeof(Instance*));
                t->target.n_instances--;

                count++;
        }

        return count;
}

static void compile_pattern_fields(
                const Transfer *t,
                const Instance *i,
                InstanceMetadata *ret) {

        assert(t);
        assert(i);
        assert(ret);

        *ret = (InstanceMetadata) {
                .version = i->metadata.version,

                /* We generally prefer explicitly configured values for the transfer over those automatically
                 * derived from the source instance. Also, if the source is a tar archive, then let's not
                 * patch mtime/mode and use the one embedded in the tar file */
                .partition_uuid = t->partition_uuid_set ? t->partition_uuid : i->metadata.partition_uuid,
                .partition_uuid_set = t->partition_uuid_set || i->metadata.partition_uuid_set,
                .partition_flags = t->partition_flags_set ? t->partition_flags : i->metadata.partition_flags,
                .partition_flags_set = t->partition_flags_set || i->metadata.partition_flags_set,
                .mtime = RESOURCE_IS_TAR(i->resource->type) ? USEC_INFINITY : i->metadata.mtime,
                .mode = t->mode != MODE_INVALID ? t->mode : (RESOURCE_IS_TAR(i->resource->type) ? MODE_INVALID : i->metadata.mode),
                .size = i->metadata.size,
                .tries_done = t->tries_done != UINT64_MAX ? t->tries_done :
                              i->metadata.tries_done != UINT64_MAX ? i->metadata.tries_done : 0,
                .tries_left = t->tries_left != UINT64_MAX ? t->tries_left :
                              i->metadata.tries_left != UINT64_MAX ? i->metadata.tries_left : 3,
                .no_auto = t->no_auto >= 0 ? t->no_auto : i->metadata.no_auto,
                .read_only = t->read_only >= 0 ? t->read_only : i->metadata.read_only,
                .growfs = t->growfs >= 0 ? t->growfs : i->metadata.growfs,
                .sha256sum_set = i->metadata.sha256sum_set,
        };

        memcpy(ret->sha256sum, i->metadata.sha256sum, sizeof(ret->sha256sum));
}

static int helper_on_exit(sd_event_source *s, const siginfo_t *si, void *userdata) {
        const char *name = userdata;
        int code;

        assert(s);
        assert(si);
        assert(name);

        if (si->si_code == CLD_EXITED) {
                code = si->si_status;
                if (code != EXIT_SUCCESS)
                        log_error("%s failed with exit status %i.", name, code);
                else
                        log_debug("%s succeeded.", name);
        } else {
                code = -EPROTO;
                if (IN_SET(si->si_code, CLD_KILLED, CLD_DUMPED))
                        log_error("%s terminated by signal %s.", name, signal_to_string(si->si_status));
                else
                        log_error("%s failed due to unknown reason.", name);
        }

        return sd_event_exit(sd_event_source_get_event(s), code);
}

struct notify_userdata {
        const Transfer *transfer;
        const Instance *instance;
        const TransferProgress callback;
        pid_t pid;
        void* userdata;
};

static int helper_on_notify(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
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
        struct notify_userdata *ctx = ASSERT_PTR(userdata);
        char* progress_str;
        unsigned progress;
        ssize_t n;
        int r;

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
        if (ucred->pid != ctx->pid) {
                log_warning("Got notification datagram from unexpected peer, ignoring.");
                return 0;
        }

        buf[n] = 0;

        progress_str = find_line_startswith(buf, "X_IMPORT_PROGRESS=");
        if (!progress_str)
                return 0;

        truncate_nl(progress_str);

        r = safe_atou(progress_str, &progress);
        if (r < 0 || progress > 100) {
                log_warning("Got invalid percent value '%s', ignoring.", progress_str);
                return 0;
        }

        return ctx->callback(ctx->transfer, ctx->instance, progress, ctx->userdata);
}

static int run_callout(
                const char *name,
                char *cmdline[],
                struct notify_userdata *userdata) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(sd_event_source_unrefp) sd_event_source *exit_source = NULL, *notify_source = NULL;
        _cleanup_close_ int fd = -EBADF;
        _cleanup_free_ char *bind_name = NULL;
        _cleanup_(sigkill_nowaitp) pid_t pid = 0;
        union sockaddr_union bsa;
        int r;

        assert(name);
        assert(cmdline);
        assert(cmdline[0]);

        r = sd_event_new(&event);
        if (r < 0)
                return r;

        /* Kill the helper & return an error if we get interrupted by a signal */
        r = sd_event_add_signal(event, NULL, SIGINT | SD_EVENT_SIGNAL_PROCMASK, NULL, INT_TO_PTR(-ECANCELED));
        if (r < 0)
                return r;
        r = sd_event_add_signal(event, NULL, SIGTERM | SD_EVENT_SIGNAL_PROCMASK, NULL, INT_TO_PTR(-ECANCELED));
        if (r < 0)
                return r;

        fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0)
                return -errno;

        if (asprintf(&bind_name, "@%" PRIx64 "/sysupdate/" PID_FMT "/notify", random_u64(), getpid_cached()) < 0)
                return log_oom();

        r = sockaddr_un_set_path(&bsa.un, bind_name);
        if (r < 0)
                return r;

        if (bind(fd, &bsa.sa, r) < 0)
                return -errno;

        r = setsockopt_int(fd, SOL_SOCKET, SO_PASSCRED, true);
        if (r < 0)
                return r;

        r = safe_fork(name, FORK_RESET_SIGNALS|FORK_DEATHSIG_SIGTERM|FORK_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                /* Child */
                if (setenv("NOTIFY_SOCKET", bind_name, 1) < 0) {
                        log_error_errno(errno, "setenv() failed: %m");
                        _exit(EXIT_FAILURE);
                }
                r = invoke_callout_binary(cmdline[0], (char *const*) cmdline);
                log_error_errno(r, "Failed to execute %s tool: %m", cmdline[0]);
                _exit(EXIT_FAILURE);
        }

        userdata->pid = pid;

        /* Quit the loop w/ when child process exits */
        r = sd_event_add_child(event, &exit_source, pid, WEXITED, helper_on_exit, (void*) name);
        if (r < 0)
                return r;

        r = sd_event_source_set_child_process_own(exit_source, true);
        if (r < 0)
                return r;
        TAKE_PID(pid);

        /* Propagate sd_notify calls */
        r = sd_event_add_io(event, &notify_source, fd, EPOLLIN, helper_on_notify, userdata);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(notify_source, "notify-socket");

        r = sd_event_source_set_io_fd_own(notify_source, true);
        if (r < 0)
                return r;
        TAKE_FD(fd);

        /* Process events until the helper quits */
        return sd_event_loop(event);
}

int transfer_acquire_instance(Transfer *t, Instance *i, TransferProgress cb, void *userdata) {
        _cleanup_free_ char *formatted_pattern = NULL, *digest = NULL;
        char offset[DECIMAL_STR_MAX(uint64_t)+1], max_size[DECIMAL_STR_MAX(uint64_t)+1];
        const char *where = NULL;
        InstanceMetadata f;
        Instance *existing;
        struct notify_userdata notify_ctx = {
                .transfer = t,
                .instance = i,
                .callback = cb,
                .userdata = userdata,
        };
        int r;

        assert(t);
        assert(i);
        assert(i->resource);
        assert(t == container_of(i->resource, Transfer, source));
        assert(cb);

        /* Does this instance already exist in the target? Then we don't need to acquire anything */
        existing = resource_find_instance(&t->target, i->metadata.version);
        if (existing) {
                log_info("No need to acquire '%s', already installed.", i->path);
                return 0;
        }

        assert(!t->final_path);
        assert(!t->temporary_path);
        assert(!strv_isempty(t->target.patterns));

        /* Format the target name using the first pattern specified */
        compile_pattern_fields(t, i, &f);
        r = pattern_format(t->target.patterns[0], &f, &formatted_pattern);
        if (r < 0)
                return log_error_errno(r, "Failed to format target pattern: %m");

        if (RESOURCE_IS_FILESYSTEM(t->target.type)) {

                if (!path_is_valid_full(formatted_pattern, /* accept_dot_dot = */ false))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Formatted pattern is not suitable as file name, refusing: %s", formatted_pattern);

                t->final_path = path_join(t->target.path, formatted_pattern);
                if (!t->final_path)
                        return log_oom();

                r = mkdir_parents(t->final_path, 0755);
                if (r < 0)
                        return log_error_errno(r, "Cannot create target directory: %m");

                r = tempfn_random(t->final_path, "sysupdate", &t->temporary_path);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate temporary target path: %m");

                where = t->final_path;
        }

        if (t->target.type == RESOURCE_PARTITION) {
                r = gpt_partition_label_valid(formatted_pattern);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine if formatted pattern is suitable as GPT partition label: %s", formatted_pattern);
                if (!r)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Formatted pattern is not suitable as GPT partition label, refusing: %s", formatted_pattern);

                r = find_suitable_partition(
                                t->target.path,
                                i->metadata.size,
                                t->target.partition_type_set ? &t->target.partition_type.uuid : NULL,
                                &t->partition_info);
                if (r < 0)
                        return r;

                xsprintf(offset, "%" PRIu64, t->partition_info.start);
                xsprintf(max_size, "%" PRIu64, t->partition_info.size);

                where = t->partition_info.device;
        }

        assert(where);

        log_info("%s Acquiring %s %s %s...", special_glyph(SPECIAL_GLYPH_DOWNLOAD), i->path, special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), where);

        if (RESOURCE_IS_URL(i->resource->type)) {
                /* For URL sources we require the SHA256 sum to be known so that we can validate the
                 * download. */

                if (!i->metadata.sha256sum_set)
                        return log_error_errno(r, "SHA256 checksum not known for download '%s', refusing.", i->path);

                digest = hexmem(i->metadata.sha256sum, sizeof(i->metadata.sha256sum));
                if (!digest)
                        return log_oom();
        }

        switch (i->resource->type) { /* Source */

        case RESOURCE_REGULAR_FILE:

                switch (t->target.type) { /* Target */

                case RESOURCE_REGULAR_FILE:

                        /* regular file → regular file (why fork off systemd-import for such a simple file
                         * copy case? implicit decompression mostly, and thus also sandboxing. Also, the
                         * importer has some tricks up its sleeve, such as sparse file generation, which we
                         * want to take benefit of, too.) */

                        r = run_callout("(sd-import-raw)",
                                        STRV_MAKE(
                                               SYSTEMD_IMPORT_PATH,
                                               "raw",
                                               "--direct",          /* just copy/unpack the specified file, don't do anything else */
                                               arg_sync ? "--sync=yes" : "--sync=no",
                                               i->path,
                                               t->temporary_path),
                                        &notify_ctx);
                        break;

                case RESOURCE_PARTITION:

                        /* regular file → partition */

                        r = run_callout("(sd-import-raw)",
                                        STRV_MAKE(
                                               SYSTEMD_IMPORT_PATH,
                                               "raw",
                                               "--direct",          /* just copy/unpack the specified file, don't do anything else */
                                               "--offset", offset,
                                               "--size-max", max_size,
                                               arg_sync ? "--sync=yes" : "--sync=no",
                                               i->path,
                                               t->target.path),
                                        &notify_ctx);
                        break;

                default:
                        assert_not_reached();
                }

                break;

        case RESOURCE_DIRECTORY:
        case RESOURCE_SUBVOLUME:
                assert(IN_SET(t->target.type, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME));

                /* directory/subvolume → directory/subvolume */

                r = run_callout("(sd-import-fs)",
                                STRV_MAKE(
                                       SYSTEMD_IMPORT_FS_PATH,
                                       "run",
                                       "--direct",          /* just untar the specified file, don't do anything else */
                                       arg_sync ? "--sync=yes" : "--sync=no",
                                       t->target.type == RESOURCE_SUBVOLUME ? "--btrfs-subvol=yes" : "--btrfs-subvol=no",
                                       i->path,
                                       t->temporary_path),
                                &notify_ctx);
                break;

        case RESOURCE_TAR:
                assert(IN_SET(t->target.type, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME));

                /* tar → directory/subvolume */

                r = run_callout("(sd-import-tar)",
                                STRV_MAKE(
                                       SYSTEMD_IMPORT_PATH,
                                       "tar",
                                       "--direct",          /* just untar the specified file, don't do anything else */
                                       arg_sync ? "--sync=yes" : "--sync=no",
                                       t->target.type == RESOURCE_SUBVOLUME ? "--btrfs-subvol=yes" : "--btrfs-subvol=no",
                                       i->path,
                                       t->temporary_path),
                                &notify_ctx);
                break;

        case RESOURCE_URL_FILE:

                switch (t->target.type) {

                case RESOURCE_REGULAR_FILE:

                        /* url file → regular file */

                        r = run_callout("(sd-pull-raw)",
                                       STRV_MAKE(
                                               SYSTEMD_PULL_PATH,
                                               "raw",
                                               "--direct",          /* just download the specified URL, don't download anything else */
                                               "--verify", digest,  /* validate by explicit SHA256 sum */
                                               arg_sync ? "--sync=yes" : "--sync=no",
                                               i->path,
                                               t->temporary_path),
                                        &notify_ctx);
                        break;

                case RESOURCE_PARTITION:

                        /* url file → partition */

                        r = run_callout("(sd-pull-raw)",
                                        STRV_MAKE(
                                               SYSTEMD_PULL_PATH,
                                               "raw",
                                               "--direct",              /* just download the specified URL, don't download anything else */
                                               "--verify", digest,      /* validate by explicit SHA256 sum */
                                               "--offset", offset,
                                               "--size-max", max_size,
                                               arg_sync ? "--sync=yes" : "--sync=no",
                                               i->path,
                                               t->target.path),
                                        &notify_ctx);
                        break;

                default:
                        assert_not_reached();
                }

                break;

        case RESOURCE_URL_TAR:
                assert(IN_SET(t->target.type, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME));

                r = run_callout("(sd-pull-tar)",
                                STRV_MAKE(
                                       SYSTEMD_PULL_PATH,
                                       "tar",
                                       "--direct",          /* just download the specified URL, don't download anything else */
                                       "--verify", digest,  /* validate by explicit SHA256 sum */
                                       t->target.type == RESOURCE_SUBVOLUME ? "--btrfs-subvol=yes" : "--btrfs-subvol=no",
                                       arg_sync ? "--sync=yes" : "--sync=no",
                                       i->path,
                                       t->temporary_path),
                                &notify_ctx);
                break;

        default:
                assert_not_reached();
        }
        if (r < 0)
                return r;

        if (RESOURCE_IS_FILESYSTEM(t->target.type)) {
                bool need_sync = false;
                assert(t->temporary_path);

                /* Apply file attributes if set */
                if (f.mtime != USEC_INFINITY) {
                        struct timespec ts;

                        timespec_store(&ts, f.mtime);

                        if (utimensat(AT_FDCWD, t->temporary_path, (struct timespec[2]) { ts, ts }, AT_SYMLINK_NOFOLLOW) < 0)
                                return log_error_errno(errno, "Failed to adjust mtime of '%s': %m", t->temporary_path);

                        need_sync = true;
                }

                if (f.mode != MODE_INVALID) {
                        /* Try with AT_SYMLINK_NOFOLLOW first, because it's the safe thing to do. Older
                         * kernels don't support that however, in that case we fall back to chmod(). Not as
                         * safe, but shouldn't be a problem, given that we don't create symlinks here. */
                        if (fchmodat(AT_FDCWD, t->temporary_path, f.mode, AT_SYMLINK_NOFOLLOW) < 0 &&
                            (!ERRNO_IS_NOT_SUPPORTED(errno) || chmod(t->temporary_path, f.mode) < 0))
                                return log_error_errno(errno, "Failed to adjust mode of '%s': %m", t->temporary_path);

                        need_sync = true;
                }

                /* Synchronize */
                if (arg_sync && need_sync) {
                        if (t->target.type == RESOURCE_REGULAR_FILE)
                                r = fsync_path_and_parent_at(AT_FDCWD, t->temporary_path);
                        else {
                                assert(IN_SET(t->target.type, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME));
                                r = syncfs_path(AT_FDCWD, t->temporary_path);
                        }
                        if (r < 0)
                                return log_error_errno(r, "Failed to synchronize file system backing '%s': %m", t->temporary_path);
                }

                t->install_read_only = f.read_only;
        }

        if (t->target.type == RESOURCE_PARTITION) {
                free_and_replace(t->partition_info.label, formatted_pattern);
                t->partition_change = PARTITION_LABEL;

                if (f.partition_uuid_set) {
                        t->partition_info.uuid = f.partition_uuid;
                        t->partition_change |= PARTITION_UUID;
                }

                if (f.partition_flags_set) {
                        t->partition_info.flags = f.partition_flags;
                        t->partition_change |= PARTITION_FLAGS;
                }

                if (f.no_auto >= 0) {
                        t->partition_info.no_auto = f.no_auto;
                        t->partition_change |= PARTITION_NO_AUTO;
                }

                if (f.read_only >= 0) {
                        t->partition_info.read_only = f.read_only;
                        t->partition_change |= PARTITION_READ_ONLY;
                }

                if (f.growfs >= 0) {
                        t->partition_info.growfs = f.growfs;
                        t->partition_change |= PARTITION_GROWFS;
                }
        }

        /* For regular file cases the only step left is to install the file in place, which install_file()
         * will do via rename(). For partition cases the only step left is to update the partition table,
         * which is done at the same place. */

        log_info("Successfully acquired '%s'.", i->path);
        return 0;
}

int transfer_install_instance(
                Transfer *t,
                Instance *i,
                const char *root) {

        int r;

        assert(t);
        assert(i);
        assert(i->resource);
        assert(t == container_of(i->resource, Transfer, source));

        if (t->temporary_path) {
                assert(RESOURCE_IS_FILESYSTEM(t->target.type));
                assert(t->final_path);

                r = install_file(AT_FDCWD, t->temporary_path,
                                 AT_FDCWD, t->final_path,
                                 INSTALL_REPLACE|
                                 (t->install_read_only > 0 ? INSTALL_READ_ONLY : 0)|
                                 (t->target.type == RESOURCE_REGULAR_FILE ? INSTALL_FSYNC_FULL : INSTALL_SYNCFS));
                if (r < 0)
                        return log_error_errno(r, "Failed to move '%s' into place: %m", t->final_path);

                log_info("Successfully installed '%s' (%s) as '%s' (%s).",
                         i->path,
                         resource_type_to_string(i->resource->type),
                         t->final_path,
                         resource_type_to_string(t->target.type));

                t->temporary_path = mfree(t->temporary_path);
        }

        if (t->partition_change != 0) {
                assert(t->target.type == RESOURCE_PARTITION);

                r = patch_partition(
                                t->target.path,
                                &t->partition_info,
                                t->partition_change);
                if (r < 0)
                        return r;

                log_info("Successfully installed '%s' (%s) as '%s' (%s).",
                         i->path,
                         resource_type_to_string(i->resource->type),
                         t->partition_info.device,
                         resource_type_to_string(t->target.type));
        }

        if (t->current_symlink) {
                _cleanup_free_ char *buf = NULL, *parent = NULL, *relative = NULL, *resolved = NULL;
                const char *link_path, *link_target;
                bool resolve_link_path = false;

                if (RESOURCE_IS_FILESYSTEM(t->target.type)) {

                        assert(t->target.path);

                        if (path_is_absolute(t->current_symlink)) {
                                link_path = t->current_symlink;
                                resolve_link_path = true;
                        } else {
                                buf = path_make_absolute(t->current_symlink, t->target.path);
                                if (!buf)
                                        return log_oom();

                                link_path = buf;
                        }

                        link_target = t->final_path;

                } else if (t->target.type == RESOURCE_PARTITION) {

                        assert(path_is_absolute(t->current_symlink));

                        link_path = t->current_symlink;
                        link_target = t->partition_info.device;

                        resolve_link_path = true;
                } else
                        assert_not_reached();

                if (resolve_link_path && root) {
                        r = chase(link_path, root, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &resolved, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to resolve current symlink path '%s': %m", link_path);

                        link_path = resolved;
                }

                if (link_target) {
                        r = path_extract_directory(link_path, &parent);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extract directory of target path '%s': %m", link_path);

                        r = path_make_relative(parent, link_target, &relative);
                        if (r < 0)
                                return log_error_errno(r, "Failed to make symlink path '%s' relative to '%s': %m", link_target, parent);

                        r = symlink_atomic(relative, link_path);
                        if (r < 0)
                                return log_error_errno(r, "Failed to update current symlink '%s' %s '%s': %m",
                                                       link_path,
                                                       special_glyph(SPECIAL_GLYPH_ARROW_RIGHT),
                                                       relative);

                        log_info("Updated symlink '%s' %s '%s'.",
                                 link_path, special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), relative);
                }
        }

        return 0;
}
