/*
 * siri.h - global methods for SiriDB.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 08-03-2016
 *
 */
#include <siri/siri.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <logger/logger.h>
#include <siri/net/clserver.h>
#include <siri/net/bserver.h>
#include <siri/parser/listener.h>
#include <siri/db/props.h>
#include <siri/db/users.h>
#include <siri/db/servers.h>
#include <siri/db/series.h>
#include <siri/db/shards.h>
#include <siri/db/buffer.h>
#include <siri/db/aggregate.h>
#include <siri/db/pools.h>
#include <strextra/strextra.h>
#include <siri/cfg/cfg.h>
#include <cfgparser/cfgparser.h>
#include <sys/stat.h>
#include <unistd.h>
#include <qpack/qpack.h>
#include <assert.h>
#include <siri/net/socket.h>


static void close_handlers(void);
static void signal_handler(uv_signal_t * req, int signum);
static int siridb_load_databases(void);
static void walk_close_handlers(uv_handle_t * handle, void * arg);

#define N_SIGNALS 3
static int signals[N_SIGNALS] = {SIGINT, SIGTERM, SIGSEGV};

siri_t siri = {
        .grammar=NULL,
        .loop=NULL,
        .siridb_list=NULL,
        .fh=NULL,
        .optimize=NULL,
        .heartbeat=NULL,
        .cfg=NULL,
        .args=NULL
};


void siri_setup_logger(void)
{
    int n;
    char lname[255];
    size_t len = strlen(siri.args->log_level);

#ifndef DEBUG
    /* force colors while debugging... */
    if (siri.args->log_colorized)
#endif
    {
        Logger.flags |= LOGGER_FLAG_COLORED;
    }

    for (n = 0; n < LOGGER_NUM_LEVELS; n++)
    {
        strcpy(lname, LOGGER_LEVEL_NAMES[n]);
        strx_lower_case(lname);
        if (strlen(lname) == len && strcmp(siri.args->log_level, lname) == 0)
        {
            logger_init(stdout, (n + 1) * 10);
            return;
        }
    }
    /* We should not get here since args should always
     * contain a valid log level
     */
    logger_init(stdout, 10);
}

static int siridb_load_databases(void)
{
    struct stat st = {0};
    DIR * db_container_path;
    struct dirent * dbpath;
    char buffer[PATH_MAX];
    cfgparser_return_t rc;
    cfgparser_t * cfgparser = NULL;
    qp_unpacker_t * unpacker = NULL;
    cfgparser_option_t * option = NULL;
    siridb_t * siridb;

    char err_msg[512];

    if (stat(siri.cfg->default_db_path, &st) == -1)
    {
        log_warning("Database directory not found, creating directory '%s'.",
                siri.cfg->default_db_path);
        if (mkdir(siri.cfg->default_db_path, 0700) == -1)
        {
            log_error("Cannot create directory '%s'.",
                    siri.cfg->default_db_path);
            return 1;
        }
    }

    if ((db_container_path = opendir(siri.cfg->default_db_path)) == NULL)
    {
        log_error("Cannot open database directory '%s'.",
                siri.cfg->default_db_path);
        return 1;
    }

    while((dbpath = readdir(db_container_path)) != NULL)
    {
        struct stat st = {0};

        if ((strlen(dbpath->d_name) == 1 &&
                    strcmp(dbpath->d_name, ".") == 0) ||
                (strlen(dbpath->d_name) >= 2 &&
                        (strncmp(dbpath->d_name, "..", 2) == 0 ||
                         strncmp(dbpath->d_name, "__", 2) == 0)))
            /* skip "." ".." and prefixed with double underscore directories */
            continue;

        if (fstatat(dirfd(db_container_path), dbpath->d_name, &st, 0) < 0)
            continue;

        if (!S_ISDIR(st.st_mode))
            continue;

        /* read database.conf */
        snprintf(buffer,
                PATH_MAX,
                "%s%s/database.conf",
                siri.cfg->default_db_path,
                dbpath->d_name);

        if (access(buffer, R_OK) == -1)
            continue;

        cfgparser = cfgparser_new();

        if ((rc = cfgparser_read(cfgparser, buffer)) != CFGPARSER_SUCCESS)
        {
            log_error("Could not read '%s': %s",
                    buffer,
                    cfgparser_errmsg(rc));
            closedir(db_container_path);
            cfgparser_free(cfgparser);
            return 1;
        }

        snprintf(buffer,
                PATH_MAX,
                "%s%s/database.dat",
                siri.cfg->default_db_path,
                dbpath->d_name);

        if ((unpacker = qp_from_file_unpacker(buffer)) == NULL)
        {
            log_error("Could not read '%s'", buffer);
            closedir(db_container_path);
            qp_free_unpacker(unpacker);
            cfgparser_free(cfgparser);
            return 1;
        }

        if (siridb_from_unpacker(
                unpacker,
                &siridb,
                err_msg))
        {
            log_error("Could not read '%s': %s", buffer, err_msg);
            closedir(db_container_path);
            qp_free_unpacker(unpacker);
            cfgparser_free(cfgparser);
            return 1;
        }

        /* append SiriDB to siridb_list and increment reference count */
        llist_append(siri.siridb_list, siridb);
        siridb_incref(siridb);

        qp_free_unpacker(unpacker);

        log_info("Start loading database: '%s'", siridb->dbname);

        /* set dbpath */
        snprintf(buffer,
                PATH_MAX,
                "%s%s/",
                siri.cfg->default_db_path,
                dbpath->d_name);

        siridb->dbpath = strdup(buffer);

        /* read buffer_path from database.conf */
        rc = cfgparser_get_option(
                    &option,
                    cfgparser,
                    "buffer",
                    "buffer_path");

        siridb->buffer_path = (
                rc == CFGPARSER_SUCCESS &&
                option->tp == CFGPARSER_TP_STRING) ?
                        strdup(option->val->string) : siridb->dbpath;

        /* free cfgparser */
        cfgparser_free(cfgparser);

        /* load users */
        if (siridb_users_load(siridb))
        {
            log_error("Could not read users for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* load servers */
        if (siridb_servers_load(siridb))
        {
            log_error("Could not read servers for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* load series */
        if (siridb_series_load(siridb))
        {
            log_error("Could not read series for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* load buffer */
        if (siridb_load_buffer(siridb))
        {
            log_error("Could not read buffer for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* open buffer */
        if (siridb_buffer_open(siridb))
        {
            log_error("Could not open buffer for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* load shards */
        if (siridb_shards_load(siridb))
        {
            log_error("Could not read shards for database '%s'", siridb->dbname);
            closedir(db_container_path);
            return 1;
        }

        /* generate pools */
        siridb_pools_gen(siridb);

        /* update series props */
        log_info("Updating series properties");
        imap32_walk(
                siridb->series_map,
                (imap32_cb_t) siridb_series_update_props,
                NULL);

        siridb->start_ts = (uint32_t) time(NULL);

        log_info("Finished loading database: '%s'", siridb->dbname);
    }
    closedir(db_container_path);

    return 0;
}

int siri_start(void)
{
    int rc;
    uv_signal_t sig[N_SIGNALS];

    /* initialize listener (set enter and exit functions) */
    siriparser_init_listener();

    /* initialize props (set props functions) */
    siridb_init_props();

    /* initialize aggregation */
    siridb_init_aggregates();

    /* load SiriDB grammar */
    siri.grammar = compile_grammar();

    /* create store for SiriDB instances */
    siri.siridb_list = llist_new();

    /* initialize file handler for shards */
    siri.fh = siri_fh_new(siri.cfg->max_open_files);

    /* load databases */
    if ((rc = siridb_load_databases()))
        return rc; //something went wrong

    /* initialize the default event loop */
    siri.loop = malloc(sizeof(uv_loop_t));
    uv_loop_init(siri.loop);

    /* bind signals to the event loop */
    for (int i = 0; i < N_SIGNALS; i++)
    {
        uv_signal_init(siri.loop, &sig[i]);
        uv_signal_start(&sig[i], signal_handler, signals[i]);
    }

    /* initialize the back-end server */
    if ((rc = sirinet_bserver_init(&siri)))
    {
        close_handlers();
        return rc; // something went wrong
    }

    /* initialize the client server */
    if ((rc = sirinet_clserver_init(&siri)))
    {
        close_handlers();
        return rc; // something went wrong
    }
    /* initialize optimize task (bind siri.optimize) */
    siri_optimize_init(&siri);

    /* initialize heart-beat task (bind siri.heartbeat) */
    siri_heartbeat_init(&siri);

    /* start the event loop */
    uv_run(siri.loop, UV_RUN_DEFAULT);

    /* quit, don't forget to run siri_free() (should be done in main) */
    return 0;
}

void siri_free(void)
{
    if (siri.loop != NULL)
    {
        int rc;
        rc = uv_loop_close(siri.loop);
        if (rc) // could be UV_EBUSY (-16) in case handlers are not closed yet
            log_error("Error occurred while closing the event loop: %d", rc);

    }

    free(siri.loop);
    free(siri.grammar);
    siri_fh_free(siri.fh);
    llist_free_cb(siri.siridb_list, (llist_cb_t) siridb_free_cb, NULL);
}

static void close_handlers(void)
{
    /* close open handlers */
    uv_walk(siri.loop, walk_close_handlers, NULL);

    /* run the loop once more so call-backs on uv_close() can run */
    uv_run(siri.loop, UV_RUN_DEFAULT);
}

static void signal_handler(uv_signal_t * req, int signum)
{
    log_debug("Asked SiriDB Server to stop (%d)", signum);

    /* cancel optimize task */
    siri_optimize_cancel();

    /* cancel heart-beat task */
    siri_heartbeat_cancel();

    uv_stop(siri.loop);

    close_handlers();
}

static void walk_close_handlers(uv_handle_t * handle, void * arg)
{
    switch (handle->type)
    {
    case UV_SIGNAL:
        uv_close(handle, NULL);
        break;
    case UV_TCP:
        /* TCP server has data set to NULL but
         * clients use data and should be freed.
         */
        uv_close(handle, (handle->data == NULL) ?
                NULL : (uv_close_cb) sirinet_socket_free);
        break;
    case UV_TIMER:
        uv_timer_stop((uv_timer_t *) handle);
        uv_close(handle, NULL);
        break;
    case UV_ASYNC:
        uv_close(handle, (uv_close_cb) free);
        break;
    default:
        log_error("Oh oh, we need to implement type %d", handle->type);
        assert(0);
    }
}
