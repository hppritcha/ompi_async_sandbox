/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"
#include "orte/runtime/orte_wait.h"

#include <string.h>

#include "opal/dss/dss.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"

#include "orte/mca/grpcomm/base/base.h"
#include "grpcomm_brks.h"


/* Static API's */
static int init(void);
static void finalize(void);
static int xcast(orte_vpid_t *vpids,
                 size_t nprocs,
                 opal_buffer_t *msg);
static int allgather(orte_grpcomm_coll_t *coll,
                     opal_buffer_t *buf);
static int brks_allgather_send_dist(orte_grpcomm_coll_t *coll, orte_vpid_t distance);
static void brks_allgather_recv_dist(int status, orte_process_name_t* sender,
                                     opal_buffer_t* buffer, orte_rml_tag_t tag,
                                     void* cbdata);
static int brks_finalize_coll(orte_grpcomm_coll_t *coll, int ret);

/* Module def */
orte_grpcomm_base_module_t orte_grpcomm_brks_module = {
    init,
    finalize,
    xcast,
    allgather
};

/**
 * Initialize the module
 */
static int init(void)
{
    /* setup recv for distance data */
    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                            ORTE_RML_TAG_ALLGATHER_BRKS,
                            ORTE_RML_PERSISTENT,
                            brks_allgather_recv_dist, NULL);
    return OPAL_SUCCESS;
}

/**
 * Finalize the module
 */
static void finalize(void)
{
    /* cancel the recv */
    orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_ALLGATHER_BRKS);

    return;
}

static int xcast(orte_vpid_t *vpids,
                 size_t nprocs,
                 opal_buffer_t *msg)
{
    return ORTE_ERR_NOT_IMPLEMENTED;
}

static int allgather(orte_grpcomm_coll_t *coll,
                     opal_buffer_t *sendbuf)
{
    OPAL_OUTPUT_VERBOSE((5, orte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:coll:bruck algo employed for %d processes",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)coll->ndmns));

    /* if we only have one proc participating, just copy the data across and return */
    if ((coll->ndmns != 0) && ((coll->ndmns & (coll->ndmns - 1)) == 0)) {
        OPAL_OUTPUT((orte_grpcomm_base_framework.framework_output,
                     "%s grpcomm:coll:bruck number of participating daemons (%d) is power 2",
                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int) coll->ndmns ));
        return ORTE_ERROR;
    }

    /* start by seeding the collection with our own data */
    opal_dss.copy_payload(&coll->bucket, sendbuf);

    /* Communication step:
     At every step i, rank r:
     - doubles the distance
     - sends message containing all data collected so far to rank r - distance
     - receives message containing all data collected so far from rank (r + distance)
     */
    /* find my position in the group of participants. This
     * value is the "rank" we will use in the algo
     */
    brks_allgather_send_dist(coll, 1);

    return ORTE_SUCCESS;
}

static int brks_allgather_send_dist(orte_grpcomm_coll_t *coll, orte_vpid_t distance) {
    orte_process_name_t peer_send, peer_recv;
    opal_buffer_t *send_buf;
    int rc;

    peer_send.jobid = ORTE_PROC_MY_NAME->jobid;
    peer_recv.jobid = ORTE_PROC_MY_NAME->jobid;

    if (1 == coll->ndmns) {
        peer_send.vpid = ORTE_PROC_MY_NAME->vpid;
        peer_recv.vpid = ORTE_PROC_MY_NAME->vpid;
    } else {
        orte_vpid_t nv, rank;
        rank = ORTE_VPID_INVALID;
        for (nv = 0; nv < coll->ndmns; nv++) {
            if (coll->dmns[nv] == ORTE_PROC_MY_NAME->vpid) {
                rank = nv;
                break;
            }
        }
        /* check for bozo case */
        if (ORTE_VPID_INVALID == rank) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return ORTE_ERR_NOT_FOUND;
        }
        /* first send my current contents */
        nv = (coll->ndmns + rank - distance) % coll->ndmns;
        peer_send.vpid = coll->dmns[nv];

        /* now setup to recv from my other partner */
        nv = (rank + distance) % coll->ndmns;
        peer_recv.vpid = coll->dmns[nv];
    }

    send_buf = OBJ_NEW(opal_buffer_t);

    /* pack the signature */
    if (OPAL_SUCCESS != (rc = opal_dss.pack(send_buf, &coll->sig, 1, ORTE_SIGNATURE))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(send_buf);
        return rc;
    }
    /* pack the current distance */
    if (OPAL_SUCCESS != (rc = opal_dss.pack(send_buf, &distance, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(send_buf);
        return rc;
    }
    /* pack the number of reported processes */
    if (OPAL_SUCCESS != (rc = opal_dss.pack(send_buf, &coll->nreported, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(send_buf);
        return rc;
    }
    /* pack the data */
    if (OPAL_SUCCESS != (rc = opal_dss.copy_payload(send_buf, &coll->bucket))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(send_buf);
        return rc;
    }

    OPAL_OUTPUT_VERBOSE((5, orte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:coll:bruck sending to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&peer_send)));

    if (0 > (rc = orte_rml.send_buffer_nb(&peer_send, send_buf,
                                          ORTE_RML_TAG_ALLGATHER_BRKS,
                                          orte_rml_send_callback, NULL))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(send_buf);
        return rc;
    };

    OPAL_OUTPUT_VERBOSE((5, orte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:coll:bruck receiving from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&peer_recv)));

    return ORTE_SUCCESS;
}

static void brks_allgather_recv_dist(int status, orte_process_name_t* sender,
                                     opal_buffer_t* buffer, orte_rml_tag_t tag,
                                     void* cbdata)
{
    int32_t cnt, num_remote;
    int rc;
    orte_grpcomm_signature_t *sig;
    orte_grpcomm_coll_t *coll;
    orte_vpid_t distance, new_distance;

    OPAL_OUTPUT_VERBOSE((5, orte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:coll:recdub received data",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* unpack the signature */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &sig, &cnt, ORTE_SIGNATURE))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = orte_grpcomm_base_get_tracker(sig, true))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        OBJ_RELEASE(sig);
        return;
    }

    /* unpack the distance */
    distance = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &distance, &cnt, OPAL_INT32))) {
        OBJ_RELEASE(sig);
        ORTE_ERROR_LOG(rc);
        brks_finalize_coll(coll, rc);
        return;
    }

    /* unpack number of reported processes */
    num_remote = 0;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(buffer, &num_remote, &cnt, OPAL_INT32))) {
        OBJ_RELEASE(sig);
        ORTE_ERROR_LOG(rc);
        brks_finalize_coll(coll, rc);
        return;
    }
    coll->nreported += num_remote;

    /* capture any provided content */
    if (OPAL_SUCCESS != (rc = opal_dss.copy_payload(&coll->bucket, buffer))) {
        OBJ_RELEASE(sig);
        ORTE_ERROR_LOG(rc);
        brks_finalize_coll(coll, rc);
        return;
    }

    //update distance and send
    new_distance = distance <<= 1;
    if (new_distance < coll->ndmns) {
        brks_allgather_send_dist(coll, new_distance);
    } else {
        brks_finalize_coll(coll, ORTE_SUCCESS);
    }

    OBJ_RELEASE(sig);

    return;
}

static int brks_finalize_coll(orte_grpcomm_coll_t *coll, int ret) {
    opal_buffer_t *reply;
    int rc;

    reply = OBJ_NEW(opal_buffer_t);

    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &coll->nreported, 1, OPAL_UINT64))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return rc;
    }
    /* transfer the collected bucket */
    opal_dss.copy_payload(reply, &coll->bucket);

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, reply, coll->cbdata);
    }

    opal_list_remove_item(&orte_grpcomm_base.ongoing, &coll->super);

    OBJ_RELEASE(reply);

    return ORTE_SUCCESS;
}
