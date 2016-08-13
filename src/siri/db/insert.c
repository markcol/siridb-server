/*
 * insert.c - Handler database inserts.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 24-03-2016
 *
 */
#include <assert.h>
#include <logger/logger.h>
#include <qpack/qpack.h>
#include <siri/async.h>
#include <siri/db/forward.h>
#include <siri/db/insert.h>
#include <siri/db/points.h>
#include <siri/db/replicate.h>
#include <siri/db/series.h>
#include <siri/err.h>
#include <siri/net/promises.h>
#include <siri/net/protocol.h>
#include <siri/net/socket.h>
#include <siri/siri.h>
#include <stdio.h>
#include <string.h>

static void INSERT_free(uv_handle_t * handle);
static void INSERT_points_to_pools(uv_async_t * handle);
static void INSERT_on_response(slist_t * promises, uv_async_t * handle);
static uint16_t INSERT_get_pool(siridb_t * siridb, qp_obj_t * qp_series_name);

static ssize_t INSERT_assign_by_map(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj);

static ssize_t INSERT_assign_by_array(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj,
        qp_packer_t * tmp_packer);

static int INSERT_read_points(
        siridb_t * siridb,
        qp_packer_t * packer,
        qp_unpacker_t * unpacker,
        qp_obj_t * qp_obj,
        ssize_t * count);

#define MAX_INSERT_MSG 236

/*
 * Return an error message for an insert err.
 */
const char * siridb_insert_err_msg(siridb_insert_err_t err)
{
    switch (err)
    {
    case ERR_EXPECTING_ARRAY:
        return  "Expecting an array with points.";
    case ERR_EXPECTING_SERIES_NAME:
        return  "Expecting a series name (string value) with an array of "
                "points where each point should be an integer time-stamp "
                "with a value.";
    case ERR_EXPECTING_MAP_OR_ARRAY:
        return   "Expecting an array or map containing series and points.";
    case ERR_EXPECTING_INTEGER_TS:
        return  "Expecting an integer value as time-stamp.";
    case ERR_TIMESTAMP_OUT_OF_RANGE:
        return  "Received at least one time-stamp which is out-of-range.";
    case ERR_UNSUPPORTED_VALUE:
        return  "Unsupported value received. (only integer, string and float "
                "values are supported).";
    case ERR_EXPECTING_AT_LEAST_ONE_POINT:
        return  "Expecting a series to have at least one point.";
    case ERR_EXPECTING_NAME_AND_POINTS:
        return  "Expecting a map with name and points.";
    case ERR_MEM_ALLOC:
        return  "Critical memory allocation error";
    default:
        assert (0);
        break;
    }
    return "Unknown err";
}

/*
 * Destroy insert.
 */
void siridb_insert_free(siridb_insert_t * insert)
{
    /* free packer */
    for (size_t n = 0; n < insert->packer_size; n++)
    {
        if (insert->packer[n] != NULL)
        {
            qp_packer_free(insert->packer[n]);
        }
    }

    /* free insert */
    free(insert);

#ifdef DEBUG
    log_debug("Free insert!, hooray!");
#endif
}

/*
 * Returns a negative value in case of an error or a value equal to zero or
 * higher representing the number of points processed.
 *
 * This function can set a SIGNAL when not enough space in the packer can be
 * allocated for the points and ERR_MEM_ALLOC will be the return value if this
 * is the case.
 */
ssize_t siridb_insert_assign_pools(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_obj_t * qp_obj,
        qp_packer_t * packer[])
{
    ssize_t rc = 0;
    qp_types_t tp;

    tp = qp_next(unpacker, NULL);

    if (qp_is_map(tp))
    {
        rc = INSERT_assign_by_map(siridb, unpacker, packer, qp_obj);
    }
    else if (qp_is_array(tp))
    {
        qp_packer_t * tmp_packer = qp_packer_new(QP_SUGGESTED_SIZE);
        if (tmp_packer != NULL)
        {
            rc = INSERT_assign_by_array(
                    siridb,
                    unpacker,
                    packer,
                    qp_obj,
                    tmp_packer);
            qp_packer_free(tmp_packer);
        }
    }
    else
    {
        rc = ERR_EXPECTING_MAP_OR_ARRAY;
    }
    return (siri_err) ? ERR_MEM_ALLOC : rc;
}

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
siridb_insert_t * siridb_insert_new(
        siridb_t * siridb,
        uint32_t pid,
        uv_stream_t * client)
{
    siridb_insert_t * insert = (siridb_insert_t *) malloc(
            sizeof(siridb_insert_t) +
            siridb->pools->len * sizeof(qp_packer_t *));

    if (insert == NULL)
    {
        ERR_ALLOC
    }
    else
    {
        insert->free_cb = INSERT_free;
        insert->ref = 1;  /* used as reference on (siri_async_t) handle */

        insert->flags = (siridb->flags & SIRIDB_FLAG_REINDEXING) ?
                INSERT_FLAG_TEST : 0;

        /* n-points will be set later to the correct value */
        insert->npoints = 0;

        /* save PID and client so we can respond to the client */
        insert->pid = pid;
        insert->client = client;

        /*
         * we keep the packer size because the number of pools might change and
         * at this point the pool->len is equal to when the insert was received
         */
        insert->packer_size = siridb->pools->len;

        /*
         * Allocate packers for sending data to pools. we allocate smaller
         * sizes in case we have a lot of pools.
         */
        uint32_t psize = QP_SUGGESTED_SIZE / ((siridb->pools->len / 4) + 1);

        for (size_t n = 0; n < siridb->pools->len; n++)
        {
            if ((insert->packer[n] = sirinet_packer_new(psize)) == NULL)
            {
                return NULL;  /* a signal is raised */
            }

            /* cannot raise a signal since enough space is allocated */
            qp_add_type(insert->packer[n], QP_MAP_OPEN);
        }
    }
    return insert;
}

/*
 * Bind n-points to insert object, lock the client and start async task.
 *
 * Returns 0 if successful or -1 and a SIGNAL is raised in case of an error.
 */
int siridb_insert_points_to_pools(siridb_insert_t * insert, size_t npoints)
{
    uv_async_t * handle = (uv_async_t *) malloc(sizeof(uv_async_t));
    if (handle == NULL)
    {
        ERR_ALLOC
        return -1;
    }

    /* bind the number of points to insert object */
    insert->npoints= npoints;

    /* lock the client */
    sirinet_socket_lock(insert->client);

    uv_async_init(siri.loop, handle, INSERT_points_to_pools);
    handle->data = (void *) insert;

    uv_async_send(handle);
    return 0;
}

static int INSERT_local_test(siridb_t * siridb, qp_unpacker_t * unpacker)
{
    qp_types_t tp;
    siridb_series_t * series;
    uint16_t pool;
    const char * series_name;
    int do_forward = 0;
    char * pt;

    siridb_forward_t * forward = siridb_forward_new(siridb);
    if (forward == NULL)
    {
        return -1;  /* signal is raised */
    }

    qp_obj_t * qp_series_name = qp_object_new();
    qp_obj_t * qp_series_ts = qp_object_new();
    qp_obj_t * qp_series_val = qp_object_new();
    if (    qp_series_name == NULL ||
            qp_series_ts == NULL ||
            qp_series_val == NULL)
    {
        qp_object_free_safe(qp_series_name);
        qp_object_free_safe(qp_series_ts);
        qp_object_free_safe(qp_series_val);
        siridb_forward_free(forward);
        return -1;
    }

    uv_mutex_lock(&siridb->series_mutex);
    uv_mutex_lock(&siridb->shards_mutex);

    qp_next(unpacker, NULL); // map
    qp_next(unpacker, qp_series_name); // first series or end
    /*
     * we check for siri_err because siridb_series_add_point()
     * should never be called twice on the same series after an
     * error has occurred.
     */
    while (!siri_err && qp_is_raw_term(qp_series_name))
    {
        series_name = qp_series_name->via->raw;
        series = (siridb_series_t *) ct_get(siridb->series, series_name);
        if (series == NULL)
        {
            /* the series does not exist so check what to do... */
            pool = siridb_lookup_sn(siridb->pools->lookup, series_name);

            if (pool == siridb->server->pool)
            {
                /*
                 * This is the correct pool so create the series and
                 * add the points.
                 */

                /* save pointer position and read series type */
                pt = unpacker->pt;
                qp_next(unpacker, NULL); // array open
                qp_next(unpacker, NULL); // first point array2
                qp_next(unpacker, NULL); // first ts
                qp_next(unpacker, qp_series_val); // first val

                /* restore pointer position */
                unpacker->pt = pt;

                series = siridb_series_new(
                        siridb,
                        series_name,
                        SIRIDB_QP_MAP2_TP(qp_series_val->tp));

                if (series == NULL ||
                    ct_add(siridb->series, series->name, series))
                {
                    log_critical("Error creating series: '%s'", series_name);
                    break;  /* signal is raised */
                }
            }
            else if (siridb->replica == NULL ||
                    siridb_series_server_id(series_name) == siridb->server->id)
            {
                /*
                 * Forward the series to the correct pool because 'this' server
                 * is responsible for the series.
                 */
                do_forward = 1;

                /* testing is not needed since we check for siri_err later */
                qp_add_raw(
                        forward->packer[pool],
                        series_name,
                        qp_series_name->len);
                qp_packer_extend_fu(forward->packer[pool], unpacker);
                qp_next(unpacker, qp_series_name);
                continue;
            }
            else
            {
                /*
                 * Skip this series since it will forwarded to the correct
                 * pool by the replica server.
                 */
                qp_skip_next(unpacker);  // array
                qp_next(unpacker, qp_series_name);
                continue;
            }
        }

        qp_next(unpacker, NULL); // array open
        qp_next(unpacker, NULL); // first point array2
        qp_next(unpacker, qp_series_ts); // first ts
        qp_next(unpacker, qp_series_val); // first val
        if (siridb_series_add_point(
                siridb,
                series,
                (uint64_t *) &qp_series_ts->via->int64,
                qp_series_val->via))
        {
            break;  /* signal is raised */
        }

        while ((tp = qp_next(unpacker, qp_series_name)) == QP_ARRAY2)
        {
            qp_next(unpacker, qp_series_ts); // ts
            qp_next(unpacker, qp_series_val); // val

            if (siridb_series_add_point(
                    siridb,
                    series,
                    (uint64_t *) &qp_series_ts->via->int64,
                    qp_series_val->via))
            {
                break;  /* signal is raised */
            }
        }

        if (tp == QP_ARRAY_CLOSE)
        {
            qp_next(unpacker, qp_series_name);
        }
    }

    uv_mutex_unlock(&siridb->series_mutex);
    uv_mutex_unlock(&siridb->shards_mutex);

    qp_object_free(qp_series_name);
    qp_object_free(qp_series_ts);
    qp_object_free(qp_series_val);

    if (!do_forward)
    {
        siridb_forward_free(forward);
    }
    else
    {
        uv_async_t * handle = (uv_async_t *) malloc(sizeof(uv_async_t));
        if (handle == NULL || siri_err)
        {
            if (handle == NULL)
            {
                ERR_ALLOC
            }
            siridb_forward_free(forward);
        }
        else
        {
            uv_async_init(siri.loop, handle, siridb_forward_points_to_pools);
            handle->data = (void *) forward;
            uv_async_send(handle);
        }
    }
    return siri_err;
}

/*
 * Return siri_err which should be 0 if all is successful. Another value is
 * critical so basically this functions should always return 0.
 *
 * (a SIGNAL will be raised in case of an error)
 */
int siridb_insert_local(siridb_t * siridb, qp_unpacker_t * unpacker, int flags)
{
    if ((flags & INSERT_FLAG_TEST) || (
            (siridb->flags & SIRIDB_FLAG_REINDEXING) &&
            (~flags & INSERT_FLAG_TESTED)))
    {
        /*
         * We can use INSERT_local_test even if 'this' server has not set
         * the REINDEXING flag yet, since this does not depend on 'prev_lookup'
         */
        return INSERT_local_test(siridb, unpacker);
    }
    qp_types_t tp;
    siridb_series_t ** series;
    qp_obj_t * qp_series_name = qp_object_new();
    qp_obj_t * qp_series_ts = qp_object_new();
    qp_obj_t * qp_series_val = qp_object_new();
    if (qp_series_name == NULL || qp_series_ts == NULL || qp_series_val == NULL)
    {
        ERR_ALLOC
        qp_object_free_safe(qp_series_name);
        qp_object_free_safe(qp_series_ts);
        qp_object_free_safe(qp_series_val);
        return -1;
    }

    uv_mutex_lock(&siridb->series_mutex);
    uv_mutex_lock(&siridb->shards_mutex);

    qp_next(unpacker, NULL); // map
    qp_next(unpacker, qp_series_name); // first series or end

    /*
     * we check for siri_err because siridb_series_add_point()
     * should never be called twice on the same series after an
     * error has occurred.
     */
    while (!siri_err && qp_is_raw_term(qp_series_name))
    {
        series = (siridb_series_t **) ct_get_sure(
                siridb->series,
                qp_series_name->via->raw);
        if (series == NULL)
        {
            log_critical(
                    "Error getting or create series: '%s'",
                    qp_series_name->via->raw);
            break;  /* signal is raised */
        }

        qp_next(unpacker, NULL); // array open
        qp_next(unpacker, NULL); // first point array2
        qp_next(unpacker, qp_series_ts); // first ts
        qp_next(unpacker, qp_series_val); // first val

        if (ct_is_empty(*series))
        {
            *series = siridb_series_new(
                    siridb,
                    qp_series_name->via->raw,
                    SIRIDB_QP_MAP2_TP(qp_series_val->tp));
            if (*series == NULL)
            {
                log_critical(
                        "Error creating series: '%s'",
                        qp_series_name->via->raw);
                break;  /* signal is raised */
            }
        }

        if (siridb_series_add_point(
                siridb,
                *series,
                (uint64_t *) &qp_series_ts->via->int64,
                qp_series_val->via))
        {
            break;  /* signal is raised */
        }

        while ((tp = qp_next(unpacker, qp_series_name)) == QP_ARRAY2)
        {
            qp_next(unpacker, qp_series_ts); // ts
            qp_next(unpacker, qp_series_val); // val
            if (siridb_series_add_point(
                    siridb,
                    *series,
                    (uint64_t *) &qp_series_ts->via->int64,
                    qp_series_val->via))
            {
                break;  /* signal is raised */
            }
        }
        if (tp == QP_ARRAY_CLOSE)
        {
            qp_next(unpacker, qp_series_name);
        }
    }

    uv_mutex_unlock(&siridb->series_mutex);
    uv_mutex_unlock(&siridb->shards_mutex);

    qp_object_free(qp_series_name);
    qp_object_free(qp_series_ts);
    qp_object_free(qp_series_val);

    return siri_err;
}

/*
 * Call-back function: sirinet_promises_cb
 *
 * This function can raise a SIGNAL.
 */
static void INSERT_on_response(slist_t * promises, uv_async_t * handle)
{
    if (promises != NULL)
    {
        sirinet_pkg_t * pkg;
        sirinet_promise_t * promise;
        siridb_insert_t * insert = (siridb_insert_t *) handle->data;
        siridb_t * siridb =
                ((sirinet_socket_t *) insert->client->data)->siridb;

        char msg[MAX_INSERT_MSG];

        /* the packer size is big enough to hold MAX_INSERT_MSG */
        qp_packer_t * packer = sirinet_packer_new(256);

        if (packer != NULL)
        {
            cproto_server_t tp = CPROTO_RES_INSERT;

            for (size_t i = 0; i < promises->len; i++)
            {
                promise = promises->data[i];
                if (siri_err || promise == NULL)
                {
                    snprintf(msg,
                            MAX_INSERT_MSG,
                            "Critical error occurred on '%s'",
                            siridb->server->name);
                    tp = CPROTO_ERR_INSERT;
                    continue;
                }
                pkg = promise->data;

                if (pkg == NULL || pkg->tp != BPROTO_ACK_INSERT)
                {
                    snprintf(msg,
                            MAX_INSERT_MSG,
                            "Error occurred while sending points to at "
                            "least '%s'",
                            promise->server->name);
                    tp = CPROTO_ERR_INSERT;
                }

                /* make sure we free the promise and data */
                free(promise->data);
                free(promise);
            }

            /* this will fit for sure */
            qp_add_type(packer, QP_MAP_OPEN);

            if (tp == CPROTO_ERR_INSERT)
            {
                qp_add_raw(packer, "error_msg", 9);
            }
            else
            {
                qp_add_raw(packer, "success_msg", 11);
                snprintf(msg,
                        MAX_INSERT_MSG,
                        "Inserted %zd point(s) successfully.",
                        insert->npoints);
                log_info(msg);
                siridb->received_points += insert->npoints;
            }

            qp_add_string(packer, msg);

            sirinet_pkg_t * response_pkg = sirinet_packer2pkg(
                    packer,
                    insert->pid,
                    tp);

            sirinet_pkg_send((uv_stream_t *) insert->client, response_pkg);
        }
    }

    uv_close((uv_handle_t *) handle, siri_async_close);
}

/*
 * Call-back function:  uv_async_cb
 *
 * In case of an error a SIGNAL is raised and a successful message will not
 * be send to the client.
 */
static void INSERT_points_to_pools(uv_async_t * handle)
{
    siridb_insert_t * insert = (siridb_insert_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) insert->client->data)->siridb;
    uint16_t pool = siridb->server->pool;
    sirinet_pkg_t * pkg;
    sirinet_promises_t * promises = sirinet_promises_new(
            siridb->pools->len - 1,
            (sirinet_promises_cb) INSERT_on_response,
            handle);

    if (promises == NULL)
    {
        return;  /* signal is raised */
    }

    int pool_count = 0;

    for (uint16_t n = 0; n < insert->packer_size; n++)
    {
        if (insert->packer[n]->len == PKG_HEADER_SIZE + 1)
        {
            /*
             * skip empty packer.
             * (empty packer has only PKG_HEADER_SIZE + QP_MAP_OPEN)
             */
            qp_packer_free(insert->packer[n]);
        }
        else if (n == pool)
        {
            if (siridb->replica != NULL)
            {
#ifdef DEBUG
                assert (siridb->fifo != NULL);
#endif
                qp_unpacker_t * unpacker;

                if (siridb->replicate->initsync == NULL)
                {
                    pkg = sirinet_packer2pkg(
                            insert->packer[n],
                            0,
                            (insert->flags & INSERT_FLAG_TEST) ?
                                BPROTO_INSERT_TEST_SERVER :
                            (insert->flags & INSERT_FLAG_TESTED) ?
                                BPROTO_INSERT_TESTED_SERVER :
                                BPROTO_INSERT_SERVER);
                }
                else
                {
                    pkg = siridb_replicate_pkg_filter(
                            siridb,
                            insert->packer[n]->buffer + PKG_HEADER_SIZE,
                            insert->packer[n]->len - PKG_HEADER_SIZE,
                            insert->flags);
                    qp_packer_free(insert->packer[n]);
                }

                insert->packer[n] = NULL;

                if (pkg != NULL)
                {
                    siridb_replicate_pkg(siridb, pkg);
                    unpacker = qp_unpacker_new(pkg->data, pkg->len);
                    if (unpacker != NULL)
                    {
                        siridb_insert_local(siridb, unpacker, insert->flags);
                        qp_unpacker_free(unpacker);
                    }
                    free(pkg);
                }
            }
            else
            {
                qp_unpacker_t * unpacker = qp_unpacker_new(
                        insert->packer[n]->buffer + PKG_HEADER_SIZE,
                        insert->packer[n]->len - PKG_HEADER_SIZE);

                /* a signal is set in case creating the unpacker fails and this
                 * signal is handled in the promises->cb function.
                 */
                if (unpacker != NULL)
                {
                    siridb_insert_local(siridb, unpacker, insert->flags);
                    qp_unpacker_free(unpacker);
                }

                qp_packer_free(insert->packer[n]);
            }
        }
        else
        {
            pkg = sirinet_packer2pkg(
                    insert->packer[n],
                    0,
                    (insert->flags & INSERT_FLAG_TEST) ?
                            BPROTO_INSERT_TEST_POOL : BPROTO_INSERT_POOL);
            if (siridb_pool_send_pkg(
                    siridb->pools->pool + n,
                    pkg,
                    0,
                    sirinet_promises_on_response,
                    promises,
                    0))
            {
                free(pkg);
                log_error(
                    "Although we have checked and validated each pool "
                    "had at least one server available, it seems that the "
                    "situation has changed and we cannot send points to "
                    "pool %u", n);
            }
            else
            {
                pool_count++;
            }
        }
        insert->packer[n] = NULL;
    }

    /* pool_count is always smaller than the initial promises->size */
    promises->promises->size = pool_count;

    SIRINET_PROMISES_CHECK(promises)
}

/*
 * Returns the correct pool.
 */
static uint16_t INSERT_get_pool(siridb_t * siridb, qp_obj_t * qp_series_name)
{
    uint16_t pool;

    if (~siridb->flags & SIRIDB_FLAG_REINDEXING)
    {
        /* when not re-indexing, select the correct pool */
        pool = siridb_lookup_sn_raw(
                siridb->pools->lookup,
                qp_series_name->via->raw,
                qp_series_name->len);
    }
    else
    {
        if (ct_getn(
                siridb->series,
                qp_series_name->via->raw,
                qp_series_name->len) != NULL)
        {
            /*
             * we are re-indexing and at least at this moment still own the
             * series
             */
            pool = siridb->server->pool;
        }
        else
        {
            /*
             * We are re-indexing and do not have the series.
             * Select the correct pool BEFORE re-indexing was
             * started or the new correct pool if this pool is
             * the previous correct pool. (we can do this now
             * because we known we don't have the series)
             */
#ifdef DEBUG
            assert (siridb->pools->prev_lookup != NULL);
#endif
            pool = siridb_lookup_sn_raw(
                    siridb->pools->prev_lookup,
                    qp_series_name->via->raw,
                    qp_series_name->len);

            if (pool == siridb->server->pool)
            {
                pool = siridb_lookup_sn_raw(
                        siridb->pools->lookup,
                        qp_series_name->via->raw,
                        qp_series_name->len);
            }
        }
    }
    return pool;
}

/*
 * Returns a negative value in case of an error or a value equal to zero or
 * higher representing the number of points processed.
 *
 * This function can set a SIGNAL when not enough space in the packer can be
 * allocated for the points and should be checked with 'siri_err'.
 */
static ssize_t INSERT_assign_by_map(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj)
{
    int tp;  /* use int instead of qp_types_t for negative values */
    uint16_t pool;
    ssize_t count = 0;

    tp = qp_next(unpacker, qp_obj);

    while ( tp == QP_RAW &&
            qp_obj->len &&
            qp_obj->len < SIRIDB_SERIES_NAME_LEN_MAX)
    {
        pool = INSERT_get_pool(siridb, qp_obj);

        qp_add_raw_term(packer[pool],
                qp_obj->via->raw,
                qp_obj->len);

        if ((tp = INSERT_read_points(
                siridb,
                packer[pool],
                unpacker,
                qp_obj,
                &count)) < 0)
        {
            return tp;
        }
    }

    if (tp != QP_END && tp != QP_MAP_CLOSE)
    {
        return ERR_EXPECTING_SERIES_NAME;
    }

    return count;
}

/*
 * Returns a negative value in case of an error or a value equal to zero or
 * higher representing the number of points processed.
 *
 * This function can set a SIGNAL when not enough space in the packer can be
 * allocated for the points and should be checked with 'siri_err'.
 */
static ssize_t INSERT_assign_by_array(
        siridb_t * siridb,
        qp_unpacker_t * unpacker,
        qp_packer_t * packer[],
        qp_obj_t * qp_obj,
        qp_packer_t * tmp_packer)
{
    int tp;  /* use int instead of qp_types_t for negative values */
    uint16_t pool;
    ssize_t count = 0;
    tp = qp_next(unpacker, qp_obj);

    while (tp == QP_MAP2)
    {
        if (qp_next(unpacker, qp_obj) != QP_RAW)
        {
            return ERR_EXPECTING_NAME_AND_POINTS;
        }

        if (strncmp(qp_obj->via->raw, "points", qp_obj->len) == 0)
        {
            if ((tp = INSERT_read_points(
                    siridb,
                    tmp_packer,
                    unpacker,
                    qp_obj,
                    &count)) < 0 || tp != QP_RAW)
            {
                return (tp < 0) ? tp : ERR_EXPECTING_NAME_AND_POINTS;
            }
        }

        if (strncmp(qp_obj->via->raw, "name", qp_obj->len) == 0)
        {
            if (    qp_next(unpacker, qp_obj) != QP_RAW ||
                    !qp_obj->len ||
                    qp_obj->len >= SIRIDB_SERIES_NAME_LEN_MAX)
            {
                return ERR_EXPECTING_NAME_AND_POINTS;
            }

            pool = INSERT_get_pool(siridb, qp_obj);

            qp_add_raw_term(packer[pool],
                    qp_obj->via->raw,
                    qp_obj->len);
        }
        else
        {
            return ERR_EXPECTING_NAME_AND_POINTS;
        }

        if (tmp_packer->len)
        {
            qp_packer_extend(packer[pool], tmp_packer);
            tmp_packer->len = 0;
            tp = qp_next(unpacker, qp_obj);
        }
        else
        {
            if (qp_next(unpacker, qp_obj) != QP_RAW ||
                    strncmp(qp_obj->via->raw, "points", qp_obj->len))
            {
                return ERR_EXPECTING_NAME_AND_POINTS;
            }

            if ((tp = INSERT_read_points(
                    siridb,
                    packer[pool],
                    unpacker,
                    qp_obj,
                    &count)) < 0)
            {
                return tp;
            }
        }
    }

    if (tp != QP_END && tp != QP_ARRAY_CLOSE)
    {
        return ERR_EXPECTING_SERIES_NAME;
    }

    return count;
}

/*
 * Returns a negative value in case of an error or a value equal to zero or
 * higher representing the next qpack type in the unpaker.
 *
 * This function can set a SIGNAL when not enough space in the packer can be
 * allocated for the points.
 */
static int INSERT_read_points(
        siridb_t * siridb,
        qp_packer_t * packer,
        qp_unpacker_t * unpacker,
        qp_obj_t * qp_obj,
        ssize_t * count)
{
    qp_types_t tp;

    if (!qp_is_array(qp_next(unpacker, NULL)))
    {
        return ERR_EXPECTING_ARRAY;
    }

    qp_add_type(packer, QP_ARRAY_OPEN);

    if ((tp = qp_next(unpacker, NULL)) != QP_ARRAY2)
    {
        return ERR_EXPECTING_AT_LEAST_ONE_POINT;
    }

    for (; tp == QP_ARRAY2; (*count)++, tp = qp_next(unpacker, qp_obj))
    {
        qp_add_type(packer, QP_ARRAY2);

        if (qp_next(unpacker, qp_obj) != QP_INT64)
        {
            return ERR_EXPECTING_INTEGER_TS;
        }

        if (!siridb_int64_valid_ts(siridb, qp_obj->via->int64))
        {
            return ERR_TIMESTAMP_OUT_OF_RANGE;
        }

        qp_add_int64(packer, qp_obj->via->int64);

        switch (qp_next(unpacker, qp_obj))
        {
        case QP_RAW:
            qp_add_raw(packer,
                    qp_obj->via->raw,
                    qp_obj->len);
            break;

        case QP_INT64:
            qp_add_int64(packer,
                    qp_obj->via->int64);
            break;

        case QP_DOUBLE:
            qp_add_double(packer,
                    qp_obj->via->real);
            break;

        default:
            return ERR_UNSUPPORTED_VALUE;
        }
    }

    if (tp == QP_ARRAY_CLOSE)
    {
        tp = qp_next(unpacker, qp_obj);
    }

    qp_add_type(packer, QP_ARRAY_CLOSE);

    return tp;
}

/*
 * Used as uv_close_cb.
 */
static void INSERT_free(uv_handle_t * handle)
{
    siridb_insert_t * insert = (siridb_insert_t *) handle->data;

    /* unlock the client */
    sirinet_socket_unlock(insert->client);

    /* free insert */
    siridb_insert_free(insert);

    /* free handle */
    free((uv_async_t *) handle);

}


