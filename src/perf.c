#include <czmq.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#include "hwinfo.h"
#include "events.h"
#include "payload.h"
#include "perf.h"
#include "util.h"
#include "report.h"

struct perf_config *
perf_config_create(struct hwinfo *hwinfo, zhashx_t *events_groups, const char *cgroup_name, const char *cgroup_path)
{
    struct perf_config *config = malloc(sizeof(struct perf_config));
    
    if (!config)
        return NULL;

    config->hwinfo = hwinfo_dup(hwinfo);
    config->events_groups = zhashx_dup(events_groups);
    config->cgroup_name = strdup(cgroup_name);
    config->cgroup_path = (cgroup_path) ? strdup(cgroup_path) : NULL;

    return config;
}

void
perf_config_destroy(struct perf_config *config)
{
    if (!config)
        return;

    hwinfo_destroy(config->hwinfo);
    zhashx_destroy(&config->events_groups);
    free(config->cgroup_name);
    free(config->cgroup_path);
    free(config);
}

static void
perf_event_fd_destroy(int **fd_ptr)
{
    close(**fd_ptr);
    free(*fd_ptr);
    *fd_ptr = NULL;
}

static struct perf_group_cpu_context *
perf_group_cpu_context_create()
{
    struct perf_group_cpu_context *ctx = malloc(sizeof(struct perf_group_cpu_context));

    if (!ctx)
        return NULL;

    ctx->perf_fds = zlistx_new();
    zlistx_set_duplicator(ctx->perf_fds, (zlistx_duplicator_fn *) intptrdup);
    zlistx_set_destructor(ctx->perf_fds, (zlistx_destructor_fn *) perf_event_fd_destroy);

    return ctx;
};

static void
perf_group_cpu_context_destroy(struct perf_group_cpu_context **ctx)
{
    if (!*ctx)
        return;

    zlistx_destroy(&(*ctx)->perf_fds);
    free(*ctx);
    *ctx = NULL;
}

static struct perf_group_pkg_context *
perf_group_pkg_context_create()
{
    struct perf_group_pkg_context *ctx = malloc(sizeof(struct perf_group_pkg_context));

    if (!ctx)
        return NULL;

    ctx->cpus_ctx = zhashx_new();
    zhashx_set_key_duplicator(ctx->cpus_ctx, (zhashx_duplicator_fn *) uintptrdup);
    zhashx_set_key_comparator(ctx->cpus_ctx, (zhashx_comparator_fn *) uintptrcmp);
    zhashx_set_key_destructor(ctx->cpus_ctx, (zhashx_destructor_fn *) ptrfree);
    zhashx_set_destructor(ctx->cpus_ctx, (zhashx_destructor_fn *) perf_group_cpu_context_destroy);

    return ctx;
}

static void
perf_group_pkg_context_destroy(struct perf_group_pkg_context **ctx)
{
    if (!*ctx)
        return;

    zhashx_destroy(&(*ctx)->cpus_ctx);
    free(*ctx);
    *ctx = NULL;
}

static struct perf_group_context *
perf_group_context_create(struct events_group *group)
{
    struct perf_group_context *ctx = malloc(sizeof(struct perf_group_context));

    if (!ctx)
        return NULL;

    ctx->config = group;
    ctx->pkgs_ctx = zhashx_new();
    zhashx_set_key_duplicator(ctx->pkgs_ctx, (zhashx_duplicator_fn *) uintptrdup);
    zhashx_set_key_comparator(ctx->pkgs_ctx, (zhashx_comparator_fn *) uintptrcmp);
    zhashx_set_key_destructor(ctx->pkgs_ctx, (zhashx_destructor_fn *) ptrfree);
    zhashx_set_destructor(ctx->pkgs_ctx, (zhashx_destructor_fn *) perf_group_pkg_context_destroy);

    return ctx;
}

static void
perf_group_context_destroy(struct perf_group_context **ctx)
{
    if (!*ctx)
        return;

    zhashx_destroy(&(*ctx)->pkgs_ctx);
    free(*ctx);
    *ctx = NULL;
}

static struct perf_context *
perf_context_create(struct perf_config *config, zsock_t *pipe)
{
    struct perf_context *ctx = malloc(sizeof(struct perf_context));

    if (!ctx)
        return NULL;

    ctx->config = config;
    ctx->terminated = false;
    ctx->pipe = pipe;
    ctx->ticker = zsock_new_sub("inproc://ticker", "CLOCK_TICK");
    ctx->poller = zpoller_new(ctx->pipe, ctx->ticker, NULL);
    ctx->reporting = zsock_new_push("inproc://reporting");
    ctx->cgroup_fd = -1; /* by default, system wide monitoring */
    ctx->groups_ctx = zhashx_new();
    zhashx_set_destructor(ctx->groups_ctx, (zhashx_destructor_fn *) perf_group_context_destroy);

    return ctx;
}

static void
perf_context_destroy(struct perf_context *ctx)
{
    if (!ctx)
        return;

    perf_config_destroy(ctx->config);
    zpoller_destroy(&ctx->poller);
    zsock_destroy(&ctx->ticker);
    zsock_destroy(&ctx->reporting);
    zhashx_destroy(&ctx->groups_ctx);
    free(ctx);
}

static int
perf_events_group_setup_cpu(struct perf_context *ctx, struct perf_group_cpu_context *cpu_ctx, struct events_group *group, int perf_flags, unsigned int cpu_id)
{
    size_t i;
    int group_fd = -1;
    int perf_fd;

    for (i = 0; i < group->events->num_attrs; i++) {
        errno = 0;
        perf_fd = perf_event_open(&group->events->attrs[i].perf_attr, ctx->cgroup_fd, cpu_id, group_fd, perf_flags);
        if (perf_fd < 1) {
            zsys_error("perf<%s>: failed opening perf event for group=%s cpu=%d event=%s errno=%d", ctx->config->cgroup_name, group->name, cpu_id, group->events->attrs[i].name, errno);
            return -1;
        }

        /* the first event is the group leader */
        if (i == 0)
            group_fd = perf_fd;

        zlistx_add_end(cpu_ctx->perf_fds, &perf_fd);
    }

    return 0;
}

static int
perf_events_groups_initialize(struct perf_context *ctx)
{
    int perf_flags = 0;
    struct events_group *events_group = NULL;
    char *events_group_name = NULL;
    struct perf_group_context *group_ctx = NULL;
    struct hwinfo_pkg *pkg = NULL;
    unsigned int *pkg_id = NULL;
    struct perf_group_pkg_context *pkg_ctx = NULL;
    unsigned int *cpu_id = NULL;
    struct perf_group_cpu_context *cpu_ctx = NULL;

    if (ctx->config->cgroup_path) {
        perf_flags |= PERF_FLAG_PID_CGROUP;
        errno = 0;
        ctx->cgroup_fd = open(ctx->config->cgroup_path, O_RDONLY); 
        if (ctx->cgroup_fd < 1) {
            zsys_error("perf<%s>: cannot open cgroup dir path=%s errno=%d", ctx->config->cgroup_name, ctx->config->cgroup_path, errno);
            return -1;
        }
    }

    for (events_group = zhashx_first(ctx->config->events_groups); events_group; events_group = zhashx_next(ctx->config->events_groups)) {
        events_group_name = (char *) zhashx_cursor(ctx->config->events_groups);

        /* create group context */
        group_ctx = perf_group_context_create(events_group);
        if (!group_ctx) {
            zsys_error("perf<%s>: failed to create context for group=%s", ctx->config->cgroup_name, events_group_name);
            return -1;
        }

        for (pkg = zhashx_first(ctx->config->hwinfo->pkgs); pkg; pkg = zhashx_next(ctx->config->hwinfo->pkgs)) {
            pkg_id = (unsigned int *) zhashx_cursor(ctx->config->hwinfo->pkgs);

            /* create package context */
            pkg_ctx = perf_group_pkg_context_create();
            if (!pkg_ctx) {
                zsys_error("perf<%s>: failed to create pkg context for group=%s pkg=%lu", ctx->config->cgroup_name, events_group_name, *pkg_id);
                return -1;
            }

            for (cpu_id = zlistx_first(pkg->cpus_id); cpu_id; cpu_id = zlistx_next(pkg->cpus_id)) {
                /* create cpu context */
                cpu_ctx = perf_group_cpu_context_create();
                if (!cpu_ctx) {
                    zsys_error("perf<%s>: failed to create cpu context for group=%s pkg=%d cpu=%d", ctx->config->cgroup_name, events_group_name, *pkg_id, *cpu_id);
                }

                /* open events of the group for the cpu */
                if (perf_events_group_setup_cpu(ctx, cpu_ctx, events_group, perf_flags, *cpu_id)) {
                    zsys_error("perf<%s>: failed to setup perf for group=%s pkg=%d cpu=%d", ctx->config->cgroup_name, events_group_name, *pkg_id, *cpu_id);
                    perf_group_cpu_context_destroy(&cpu_ctx);
                    return -1;
                }

                /* store cpu context */
                zhashx_insert(pkg_ctx->cpus_ctx, cpu_id, cpu_ctx);

                if (events_group->type == MONITOR_ONE_CPU_PER_SOCKET)
                    break;
            }

            /* store pkg context */
            zhashx_insert(group_ctx->pkgs_ctx, pkg_id, pkg_ctx);
        }

        /* stores per-cpu events fd for group */
        zhashx_insert(ctx->groups_ctx, events_group_name, group_ctx);
    }

    return 0;
}

static void
perf_events_groups_enable(struct perf_context *ctx)
{
    struct perf_group_context *group_ctx = NULL;
    char *group_name = NULL;
    struct perf_group_pkg_context *pkg_ctx = NULL;
    unsigned int *pkg_id = NULL;
    struct perf_group_cpu_context *cpu_ctx = NULL;
    unsigned int *cpu_id = NULL;
    int *group_leader_fd = NULL;

    for (group_ctx = zhashx_first(ctx->groups_ctx); group_ctx; group_ctx = zhashx_next(ctx->groups_ctx)) {
        group_name = (char *) zhashx_cursor(ctx->groups_ctx);

        for (pkg_ctx = zhashx_first(group_ctx->pkgs_ctx); pkg_ctx; pkg_ctx = zhashx_next(group_ctx->pkgs_ctx)) {
            pkg_id = (unsigned int *) zhashx_cursor(group_ctx->pkgs_ctx);

            for (cpu_ctx = zhashx_first(pkg_ctx->cpus_ctx); cpu_ctx; cpu_ctx = zhashx_next(pkg_ctx->cpus_ctx)) {
                cpu_id = (unsigned int *) zhashx_cursor(pkg_ctx->cpus_ctx);
                group_leader_fd = zlistx_first(cpu_ctx->perf_fds);

                errno = 0;
                if (ioctl(*group_leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP))
                    zsys_error("perf<%s>: cannot reset events for group=%s pkg=%lu cpu=%lu errno=%d", ctx->config->cgroup_name, group_name, *pkg_id, *cpu_id, errno);

                errno = 0;
                if (ioctl(*group_leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP))
                    zsys_error("perf<%s>: cannot enable events for group=%s pkg=%lu cpu=%lu errno=%d", ctx->config->cgroup_name, group_name, *pkg_id, *cpu_id, errno);
            }
        }
    }
}

static int
perf_events_group_read_cpu(struct perf_group_cpu_context *cpu_ctx, struct perf_read_format *buffer, size_t buffer_size)
{
    int *group_leader_fd = NULL;

    group_leader_fd = zlistx_first(cpu_ctx->perf_fds);
    if (!group_leader_fd)
        return -1;

    if (read(*group_leader_fd, buffer, buffer_size) != (ssize_t) buffer_size)
        return -1;

    /* counters need to be reset in order to count the events per tick */
    if (ioctl(*group_leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP))
        return -1;

    return 0;
}

static void
handle_pipe(struct perf_context *ctx)
{
    char *command = zstr_recv(ctx->pipe);
    
    if (streq(command, "$TERM")) {
        ctx->terminated = true;
        zsys_info("perf<%s>: bye!", ctx->config->cgroup_name);
    }
    else
        zsys_error("perf<%s>: invalid pipe command: %s", ctx->config->cgroup_name, command);

    zstr_free(&command);
}

static inline double
compute_perf_multiplexing_ratio(struct perf_read_format *report)
{
    return (!report->time_enabled) ? 0.0 : (double) report->time_running / (double) report->time_enabled;
}

static int
populate_payload(struct perf_context *ctx, struct payload *payload)
{
    struct perf_group_context *group_ctx = NULL;
    char *group_name = NULL;
    struct payload_group_data *group_data = NULL;
    struct perf_group_pkg_context *pkg_ctx = NULL;
    unsigned int *pkg_id = NULL;
    struct payload_pkg_data *pkg_data = NULL;
    struct perf_group_cpu_context *cpu_ctx = NULL;
    unsigned int *cpu_id = NULL;
    struct payload_cpu_data *cpu_data = NULL;
    size_t perf_read_buffer_size;
    struct perf_read_format *perf_read_buffer = NULL;
    double perf_multiplexing_ratio;
    size_t event_i;

    for (group_ctx = zhashx_first(ctx->groups_ctx); group_ctx; group_ctx = zhashx_next(ctx->groups_ctx)) {
        group_name = (char *) zhashx_cursor(ctx->groups_ctx);
        group_data = payload_group_data_create();
        if (!group_data) {
            zsys_error("perf<%s>: failed to allocate group data for group=%s", ctx->config->cgroup_name, group_name);
            goto error;
        }

        /* shared perf read buffer */
        perf_read_buffer_size = offsetof(struct perf_read_format, values) + sizeof(struct perf_counter_value[group_ctx->config->events->num_attrs]);
        perf_read_buffer = malloc(perf_read_buffer_size);
        if (!perf_read_buffer) {
            zsys_error("perf<%s>: failed to allocate perf read buffer for group=%s", ctx->config->cgroup_name, group_name);
            goto error;
        }

        for (pkg_ctx = zhashx_first(group_ctx->pkgs_ctx); pkg_ctx; pkg_ctx = zhashx_next(group_ctx->pkgs_ctx)) {
            pkg_id = (unsigned int *) zhashx_cursor(group_ctx->pkgs_ctx);
            pkg_data = payload_pkg_data_create();
            if (!pkg_data) {
                zsys_error("perf<%s>: failed to allocate pkg data for group=%s pkg=%lu", ctx->config->cgroup_name, group_name, *pkg_id);
                goto error;
            }

            for (cpu_ctx = zhashx_first(pkg_ctx->cpus_ctx); cpu_ctx; cpu_ctx = zhashx_next(pkg_ctx->cpus_ctx)) {
                cpu_id = (unsigned int *) zhashx_cursor(pkg_ctx->cpus_ctx);
                cpu_data = payload_cpu_data_create();
                if (!cpu_data) {
                    zsys_error("perf<%s>: failed to allocate cpu data for group=%s pkg=%lu cpu=%lu", ctx->config->cgroup_name, group_name, *pkg_id, *cpu_id);
                    goto error;
                }

                /* read counters value for the cpu */
                if (perf_events_group_read_cpu(cpu_ctx, perf_read_buffer, perf_read_buffer_size)) {
                    zsys_error("perf<%s>: cannot read perf values for group=%s pkg=%lu cpu=%lu", ctx->config->cgroup_name, group_name, *pkg_id, *cpu_id);
                    goto error;
                }

                /* warn if PMU multiplexing is happening */
                perf_multiplexing_ratio = compute_perf_multiplexing_ratio(perf_read_buffer);
                if (perf_multiplexing_ratio < 1.0) {
                    zsys_warning("perf<%s>: perf multiplexing for group=%s pkg=%lu cpu=%lu ratio=%ld", ctx->config->cgroup_name, group_name, *pkg_id, *cpu_id, perf_multiplexing_ratio);
                }

                /* store events value */
                zhashx_insert(cpu_data->events, "time_enabled", &perf_read_buffer->time_enabled);
                zhashx_insert(cpu_data->events, "time_running", &perf_read_buffer->time_running);
                for (event_i = 0; event_i < group_ctx->config->events->num_attrs; event_i++) {
                    zhashx_insert(cpu_data->events, group_ctx->config->events->attrs[event_i].name, &perf_read_buffer->values[event_i].value);
                }
            }

            zhashx_insert(pkg_data->cpus, cpu_id, cpu_data);
        }

        free(perf_read_buffer);
        perf_read_buffer = NULL;
        zhashx_insert(payload->groups, group_name, group_data);
    }

    return 0;

error:
    free(perf_read_buffer);
    payload_cpu_data_destroy(&cpu_data);
    payload_pkg_data_destroy(&pkg_data);
    payload_group_data_destroy(&group_data);
    return -1;
}

static void
handle_ticker(struct perf_context *ctx)
{
    uint64_t timestamp;
    struct payload *payload = NULL;
    
    /* get tick timestamp */
    zsock_recv(ctx->ticker, "s8", NULL, &timestamp);

    payload = payload_create(timestamp, ctx->config->cgroup_name);
    if (!payload) {
        zsys_error("perf<%s>: failed to allocate payload for timestamp=%lu", ctx->config->cgroup_name, timestamp);
        return;
    }

    if (populate_payload(ctx, payload)) {
        zsys_error("perf<%s>: failed to populate payload for timestamp=%lu", ctx->config->cgroup_name, timestamp);
        payload_destroy(payload);
        return;
    }

    /* send payload to reporting socket */
    zsock_send(ctx->reporting, "p", payload);
}

void
perf_monitoring_actor(zsock_t *pipe, void *args)
{
    struct perf_context *ctx = perf_context_create(args, pipe);
    zsock_t *which = NULL;

    if (!ctx) {
        zsys_error("perf<%s>: cannot create perf context", ctx->config->cgroup_name);
        free(args);
        return;
    }

    zsock_signal(pipe, 0);

    if (perf_events_groups_initialize(ctx)) {
        zsys_error("perf<%s>: cannot initialize perf monitoring", ctx->config->cgroup_name);
        perf_context_destroy(ctx);
        return;
    }

    perf_events_groups_enable(ctx);

    while (!ctx->terminated) {
        which = zpoller_wait(ctx->poller, -1);

        if (zpoller_terminated(ctx->poller))
            break;
       
        if (which == ctx->pipe)
            handle_pipe(ctx);
        else if (which == ctx->ticker)
            handle_ticker(ctx);
    }

    perf_context_destroy(ctx);
}

