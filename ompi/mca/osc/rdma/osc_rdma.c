/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2014 Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * Copyright (c) 2012-2013 Sandia National Laboratories.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include "osc_rdma.h"

#include "opal/threads/mutex.h"
#include "opal/mca/btl/btl.h"
#include "ompi/win/win.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/osc/base/base.h"
#include "mpi.h"


int
ompi_osc_rdma_attach(struct ompi_win_t *win, void *base, size_t len)
{
    return OMPI_SUCCESS;
}


int
ompi_osc_rdma_detach(struct ompi_win_t *win, void *base)
{
    return OMPI_SUCCESS;
}


int
ompi_osc_rdma_free(ompi_win_t *win)
{
    int ret = OMPI_SUCCESS;
    ompi_osc_rdma_module_t *module = GET_MODULE(win);
    opal_list_item_t *item;

    if (NULL == module) {
        return OMPI_SUCCESS;
    }

    if (NULL != module->comm) {
        opal_output_verbose(1, ompi_osc_base_framework.framework_output,
                            "rdma component destroying window with id %d",
                            ompi_comm_get_cid(module->comm));

        /* finish with a barrier */
        if (ompi_group_size(win->w_group) > 1) {
            ret = module->comm->c_coll.coll_barrier(module->comm,
                                                    module->comm->c_coll.coll_barrier_module);
        }

        /* remove from component information */
        OPAL_THREAD_LOCK(&mca_osc_rdma_component.lock);
        opal_hash_table_remove_value_uint32(&mca_osc_rdma_component.modules,
                                            ompi_comm_get_cid(module->comm));
        OPAL_THREAD_UNLOCK(&mca_osc_rdma_component.lock);
    }

    win->w_osc_module = NULL;

    OBJ_DESTRUCT(&module->outstanding_locks);
    OBJ_DESTRUCT(&module->locks_pending);
    OBJ_DESTRUCT(&module->acc_lock);
    OBJ_DESTRUCT(&module->cond);
    OBJ_DESTRUCT(&module->lock);

    /* it is erroneous to close a window with active operations on it so we should
     * probably produce an error here instead of cleaning up */
    while (NULL != (item = opal_list_remove_first (&module->pending_acc))) {
        OBJ_RELEASE(item);
    }

    OBJ_DESTRUCT(&module->pending_acc);

    while (NULL != (item = opal_list_remove_first (&module->pending_posts))) {
        OBJ_RELEASE(item);
    }

    OBJ_DESTRUCT(&module->pending_posts);

    osc_rdma_gc_clean ();

    if (NULL != module->peers) {
        free(module->peers);
    }
    if (NULL != module->passive_eager_send_active) free(module->passive_eager_send_active);
    if (NULL != module->passive_incoming_frag_count) free(module->passive_incoming_frag_count);
    if (NULL != module->passive_incoming_frag_signal_count) free(module->passive_incoming_frag_signal_count);
    if (NULL != module->epoch_outgoing_frag_count) free(module->epoch_outgoing_frag_count);
    if (NULL != module->frag_request) {
        module->frag_request->req_complete_cb = NULL;
        ompi_request_cancel (module->frag_request);
        ompi_request_free (&module->frag_request);
    }
    if (NULL != module->comm) {
        ompi_comm_free(&module->comm);
    }
    if (NULL != module->incoming_buffer) free (module->incoming_buffer);
    if (NULL != module->free_after) free(module->free_after);

    free (module);

    return ret;
}
