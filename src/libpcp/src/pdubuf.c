/*
 * Copyright (c) 1995 Silicon Graphics, Inc.  All Rights Reserved.
 * Copyright (c) 2015 Red Hat, Inc.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * Thread-safe notes
 *
 * To avoid buffer trampling, on success __pmFindPDUBuf() now returns
 * a pinned PDU buffer.  It is the caller's responsibility to unpin the
 * PDU buffer when safe to do so.
 */

#include "pmapi.h"
#include "impl.h"
#include <assert.h>
#include <search.h>


/* Microoptimize with branch prediction hints. */
#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x),1)
#define unlikely(x)     __builtin_expect(!!(x),0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif


typedef struct bufctl
{
    int bc_pincnt;
    int bc_size;
    char *bc_buf;
    /* The actual buffer happens to follow this struct. */
} bufctl_t;


/* Protected by global __pmLock_libpcp. */
static void *buf_tree = NULL;


#ifdef PCP_DEBUG
static void
pdubufdump1(const void *nodep, const VISIT which, const int depth)
{
    const bufctl_t *pcp = *(bufctl_t **) nodep;
    if (which == postorder || which == leaf)	/* called once per node */
	fprintf(stderr, " " PRINTF_P_PFX "%p[%d](%d)", pcp->bc_buf, pcp->bc_size, pcp->bc_pincnt);
}


static void
pdubufdump(void)
{
    PM_LOCK(__pmLock_libpcp);
    twalk(buf_tree, &pdubufdump1);
    PM_UNLOCK(__pmLock_libpcp);
    fprintf(stderr, "\n");
}
#endif


/* A tsearch(3) comparison function for the buffer-segments used here. */
static int
bufctl_t_compare(const void *a, const void *b)
{
    const bufctl_t *aa = (const bufctl_t *) a;
    const bufctl_t *bb = (const bufctl_t *) b;
    int res;

    if ((uintptr_t) & aa->bc_buf[aa->bc_size] < (uintptr_t) & bb->bc_buf[0])
	res = -1;
    else if ((uintptr_t) & bb->bc_buf[bb->bc_size] < (uintptr_t) & aa->bc_buf[0])
	res = 1;
    else
	res = 0;		/* overlap */

    return res;
}


__pmPDU *
__pmFindPDUBuf(int need)
{
    bufctl_t *pcp;
    __pmPDU *sts;
    void *sts2;

    PM_INIT_LOCKS();
    
    if (unlikely(need < 0)) {
	/* special diagnostic case ... dump buffer state */
#ifdef PCP_DEBUG
	fprintf(stderr, "__pmFindPDUBuf(DEBUG)\n");
	pdubufdump();
#endif
	return NULL;
    }

    if ((pcp = (bufctl_t *) malloc(sizeof(*pcp)+need)) == NULL) {
	return NULL;
    }

    pcp->bc_pincnt = 1;
    pcp->bc_size = need;
    pcp->bc_buf = ((char*)pcp) + sizeof(*pcp);

    PM_LOCK(__pmLock_libpcp);
    /* Insert the node in the tree. */
    sts2 = tsearch((void *) pcp, &buf_tree, &bufctl_t_compare);
    if (unlikely(sts2 == NULL)) {		/* ENOMEM */
	PM_UNLOCK(__pmLock_libpcp);
	free(pcp);
	return NULL;
    }
    PM_UNLOCK(__pmLock_libpcp);

#ifdef PCP_DEBUG
    if (unlikely(pmDebug & DBG_TRACE_PDUBUF)) {
	fprintf(stderr, "__pmFindPDUBuf(%d) -> " PRINTF_P_PFX "%p\n", need, pcp->bc_buf);
        pdubufdump();
    }
#endif

    sts = (__pmPDU *) pcp->bc_buf;
    return sts;
}


void
__pmPinPDUBuf(void *handle)
{
    bufctl_t *pcp, pcp_search;
    void *res;

    assert(((__psint_t) handle % sizeof(int)) == 0);
    PM_INIT_LOCKS();
    PM_LOCK(__pmLock_libpcp);

    /* Initialize a dummy bufctl_t to use only as search key;
       only its bc_buf & bc_size fields need to be set, as that's
       all that bufctl_t_compare will look at. */
    pcp_search.bc_buf = handle;
    pcp_search.bc_size = 1;

    res = tfind(&pcp_search, &buf_tree, &bufctl_t_compare);
    pcp = (res ? (*(bufctl_t **) res) : NULL);
    
    /* NB: don't release the lock until final disposition of this object;
       we don't want to play TOCTOU. */

    if (likely(pcp != NULL))
	pcp->bc_pincnt++;
    else {
	__pmNotifyErr(LOG_WARNING, "__pmPinPDUBuf: 0x%lx not in pool!", (unsigned long) handle);
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_PDUBUF)
	    pdubufdump();
#endif
	PM_UNLOCK(__pmLock_libpcp);
	return;
    }

#ifdef PCP_DEBUG
    if (unlikely(pmDebug & DBG_TRACE_PDUBUF))
	fprintf(stderr, "__pmPinPDUBuf(" PRINTF_P_PFX "%p) -> pdubuf=" PRINTF_P_PFX "%p, pincnt=%d\n", handle,
		pcp->bc_buf, pcp->bc_pincnt);
#endif

    PM_UNLOCK(__pmLock_libpcp);
    return;
}


int
__pmUnpinPDUBuf(void *handle)
{
    bufctl_t *pcp, pcp_search;
    void *res;

    assert(((__psint_t) handle % sizeof(int)) == 0);
    PM_INIT_LOCKS();
    PM_LOCK(__pmLock_libpcp);

    /* Initialize a dummy bufctl_t to use only as search key;
       only its bc_buf & bc_size fields need to be set, as that's
       all that bufctl_t_compare will look at. */
    pcp_search.bc_buf = handle;
    pcp_search.bc_size = 1;

    res = tfind(&pcp_search, &buf_tree, &bufctl_t_compare);
    pcp = (res ? (*(bufctl_t **) res) : NULL);
    
    /* NB: don't release the lock until final disposition of this object;
       we don't want to play TOCTOU. */
    
    if (unlikely(pcp == NULL)) {
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_PDUBUF) {
	    fprintf(stderr, "__pmUnpinPDUBuf(" PRINTF_P_PFX "%p) -> fails\n", handle);
	    pdubufdump();
	}
#endif
	PM_UNLOCK(__pmLock_libpcp);
	return 0;
    }

#ifdef PCP_DEBUG
    if (unlikely(pmDebug & DBG_TRACE_PDUBUF))
	fprintf(stderr, "__pmUnpinPDUBuf(" PRINTF_P_PFX "%p) -> pdubuf=" PRINTF_P_PFX "%p, pincnt=%d\n", handle,
		pcp->bc_buf, pcp->bc_pincnt - 1);
#endif

    if (likely(--pcp->bc_pincnt == 0)) {
	tdelete(pcp, &buf_tree, &bufctl_t_compare);
        PM_UNLOCK(__pmLock_libpcp);
	free(pcp);
    } else {
        PM_UNLOCK(__pmLock_libpcp);
    }

    return 1;
}


/* Used to pass context from __pmCountPDUBuf to this callback function.
   They are protected by the __pmLock_libpcp. */
static int pdu_bufcnt_need;
static unsigned pdu_bufcnt;


static void
pdubufcount(const void *nodep, const VISIT which, const int depth)
{
    const bufctl_t *pcp = *(bufctl_t **) nodep;
    if (which == postorder || which == leaf)	/* called once per node */
	if (pcp->bc_size >= pdu_bufcnt_need)
	    pdu_bufcnt++;
}



void
__pmCountPDUBuf(int need, int *alloc, int *free)
{
    PM_INIT_LOCKS();
    PM_LOCK(__pmLock_libpcp);

    pdu_bufcnt_need = need;
    pdu_bufcnt = 0;
    twalk(buf_tree, &pdubufcount);
    *alloc = pdu_bufcnt;

    *free = 0;			/* We don't retain freed nodes. */

    PM_UNLOCK(__pmLock_libpcp);
    return;
}
