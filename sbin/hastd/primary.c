/*-
 * Copyright (c) 2009 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Pawel Jakub Dawidek under sponsorship from
 * the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/time.h>
#include <sys/bio.h>
#include <sys/disk.h>
#include <sys/refcount.h>
#include <sys/stat.h>

#include <geom/gate/g_gate.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgeom.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <activemap.h>
#include <nv.h>
#include <rangelock.h>

#include "control.h"
#include "hast.h"
#include "hast_proto.h"
#include "hastd.h"
#include "metadata.h"
#include "proto.h"
#include "pjdlog.h"
#include "subr.h"
#include "synch.h"

struct hio {
	/*
	 * Number of components we are still waiting for.
	 * When this field goes to 0, we can send the request back to the
	 * kernel. Each component has to decrease this counter by one
	 * even on failure.
	 */
	unsigned int		 hio_countdown;
	/*
	 * Each component has a place to store its own error.
	 * Once the request is handled by all components we can decide if the
	 * request overall is successful or not.
	 */
	int			*hio_errors;
	/*
	 * Structure used to comunicate with GEOM Gate class.
	 */
	struct g_gate_ctl_io	 hio_ggio;
	TAILQ_ENTRY(hio)	*hio_next;
};
#define	hio_free_next	hio_next[0]
#define	hio_done_next	hio_next[0]

/*
 * Free list holds unused structures. When free list is empty, we have to wait
 * until some in-progress requests are freed.
 */
static TAILQ_HEAD(, hio) hio_free_list;
static pthread_mutex_t hio_free_list_lock;
static pthread_cond_t hio_free_list_cond;
/*
 * There is one send list for every component. One requests is placed on all
 * send lists - each component gets the same request, but each component is
 * responsible for managing his own send list.
 */
static TAILQ_HEAD(, hio) *hio_send_list;
static pthread_mutex_t *hio_send_list_lock;
static pthread_cond_t *hio_send_list_cond;
/*
 * There is one recv list for every component, although local components don't
 * use recv lists as local requests are done synchronously.
 */
static TAILQ_HEAD(, hio) *hio_recv_list;
static pthread_mutex_t *hio_recv_list_lock;
static pthread_cond_t *hio_recv_list_cond;
/*
 * Request is placed on done list by the slowest component (the one that
 * decreased hio_countdown from 1 to 0).
 */
static TAILQ_HEAD(, hio) hio_done_list;
static pthread_mutex_t hio_done_list_lock;
static pthread_cond_t hio_done_list_cond;
/*
 * Structure below are for interaction with sync thread.
 */
static bool sync_inprogress;
static pthread_mutex_t sync_lock;
static pthread_cond_t sync_cond;
/*
 * The lock below allows to synchornize access to remote connections.
 */
static pthread_rwlock_t *hio_remote_lock;
static pthread_mutex_t hio_guard_lock;
static pthread_cond_t hio_guard_cond;

/*
 * Lock to synchronize metadata updates. Also synchronize access to
 * hr_primary_localcnt and hr_primary_remotecnt fields.
 */
static pthread_mutex_t metadata_lock;

/*
 * Maximum number of outstanding I/O requests.
 */
#define	HAST_HIO_MAX	256
/*
 * Number of components. At this point there are only two components: local
 * and remote, but in the future it might be possible to use multiple local
 * and remote components.
 */
#define	HAST_NCOMPONENTS	2
/*
 * Number of seconds to sleep before next reconnect try.
 */
#define	RECONNECT_SLEEP		5

#define	ISCONNECTED(res, no)	\
	((res)->hr_remotein != NULL && (res)->hr_remoteout != NULL)

#define	QUEUE_INSERT1(hio, name, ncomp)	do {				\
	bool _wakeup;							\
									\
	mtx_lock(&hio_##name##_list_lock[(ncomp)]);			\
	_wakeup = TAILQ_EMPTY(&hio_##name##_list[(ncomp)]);		\
	TAILQ_INSERT_TAIL(&hio_##name##_list[(ncomp)], (hio),		\
	    hio_next[(ncomp)]);						\
	mtx_unlock(&hio_##name##_list_lock[ncomp]);			\
	if (_wakeup)							\
		cv_signal(&hio_##name##_list_cond[(ncomp)]);		\
} while (0)
#define	QUEUE_INSERT2(hio, name)	do {				\
	bool _wakeup;							\
									\
	mtx_lock(&hio_##name##_list_lock);				\
	_wakeup = TAILQ_EMPTY(&hio_##name##_list);			\
	TAILQ_INSERT_TAIL(&hio_##name##_list, (hio), hio_##name##_next);\
	mtx_unlock(&hio_##name##_list_lock);				\
	if (_wakeup)							\
		cv_signal(&hio_##name##_list_cond);			\
} while (0)
#define	QUEUE_TAKE1(hio, name, ncomp)	do {				\
	mtx_lock(&hio_##name##_list_lock[(ncomp)]);			\
	while (((hio) = TAILQ_FIRST(&hio_##name##_list[(ncomp)])) == NULL) { \
		cv_wait(&hio_##name##_list_cond[(ncomp)],		\
		    &hio_##name##_list_lock[(ncomp)]);			\
	}								\
	TAILQ_REMOVE(&hio_##name##_list[(ncomp)], (hio),		\
	    hio_next[(ncomp)]);						\
	mtx_unlock(&hio_##name##_list_lock[(ncomp)]);			\
} while (0)
#define	QUEUE_TAKE2(hio, name)	do {					\
	mtx_lock(&hio_##name##_list_lock);				\
	while (((hio) = TAILQ_FIRST(&hio_##name##_list)) == NULL) {	\
		cv_wait(&hio_##name##_list_cond,			\
		    &hio_##name##_list_lock);				\
	}								\
	TAILQ_REMOVE(&hio_##name##_list, (hio), hio_##name##_next);	\
	mtx_unlock(&hio_##name##_list_lock);				\
} while (0)

#define	SYNCREQ(hio)		do { (hio)->hio_ggio.gctl_unit = -1; } while (0)
#define	ISSYNCREQ(hio)		((hio)->hio_ggio.gctl_unit == -1)
#define	SYNCREQDONE(hio)	do { (hio)->hio_ggio.gctl_unit = -2; } while (0)
#define	ISSYNCREQDONE(hio)	((hio)->hio_ggio.gctl_unit == -2)

static struct hast_resource *gres;

static pthread_mutex_t range_lock;
static struct rangelocks *range_regular;
static bool range_regular_wait;
static pthread_cond_t range_regular_cond;
static struct rangelocks *range_sync;
static bool range_sync_wait;
static pthread_cond_t range_sync_cond;

static void *ggate_recv_thread(void *arg);
static void *local_send_thread(void *arg);
static void *remote_send_thread(void *arg);
static void *remote_recv_thread(void *arg);
static void *ggate_send_thread(void *arg);
static void *sync_thread(void *arg);
static void *guard_thread(void *arg);

static void sighandler(int sig);

static void
cleanup(struct hast_resource *res)
{
	int rerrno;

	/* Remember errno. */
	rerrno = errno;

	/*
	 * Close descriptor to /dev/hast/<name>
	 * to work-around race in the kernel.
	 */
	close(res->hr_localfd);

	/* Destroy ggate provider if we created one. */
	if (res->hr_ggateunit >= 0) {
		struct g_gate_ctl_destroy ggiod;

		ggiod.gctl_version = G_GATE_VERSION;
		ggiod.gctl_unit = res->hr_ggateunit;
		ggiod.gctl_force = 1;
		if (ioctl(res->hr_ggatefd, G_GATE_CMD_DESTROY, &ggiod) < 0) {
			pjdlog_warning("Unable to destroy hast/%s device",
			    res->hr_provname);
		}
		res->hr_ggateunit = -1;
	}

	/* Restore errno. */
	errno = rerrno;
}

static void
primary_exit(int exitcode, const char *fmt, ...)
{
	va_list ap;

	assert(exitcode != EX_OK);
	va_start(ap, fmt);
	pjdlogv_errno(LOG_ERR, fmt, ap);
	va_end(ap);
	cleanup(gres);
	exit(exitcode);
}

static void
primary_exitx(int exitcode, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	pjdlogv(exitcode == EX_OK ? LOG_INFO : LOG_ERR, fmt, ap);
	va_end(ap);
	cleanup(gres);
	exit(exitcode);
}

static int
hast_activemap_flush(struct hast_resource *res)
{
	const unsigned char *buf;
	size_t size;

	buf = activemap_bitmap(res->hr_amp, &size);
	assert(buf != NULL);
	assert((size % res->hr_local_sectorsize) == 0);
	if (pwrite(res->hr_localfd, buf, size, METADATA_SIZE) !=
	    (ssize_t)size) {
		KEEP_ERRNO(pjdlog_errno(LOG_ERR,
		    "Unable to flush activemap to disk"));
		return (-1);
	}
	return (0);
}

static void
init_environment(struct hast_resource *res __unused)
{
	struct hio *hio;
	unsigned int ii, ncomps;

	/*
	 * In the future it might be per-resource value.
	 */
	ncomps = HAST_NCOMPONENTS;

	/*
	 * Allocate memory needed by lists.
	 */
	hio_send_list = malloc(sizeof(hio_send_list[0]) * ncomps);
	if (hio_send_list == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for send lists.",
		    sizeof(hio_send_list[0]) * ncomps);
	}
	hio_send_list_lock = malloc(sizeof(hio_send_list_lock[0]) * ncomps);
	if (hio_send_list_lock == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for send list locks.",
		    sizeof(hio_send_list_lock[0]) * ncomps);
	}
	hio_send_list_cond = malloc(sizeof(hio_send_list_cond[0]) * ncomps);
	if (hio_send_list_cond == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for send list condition variables.",
		    sizeof(hio_send_list_cond[0]) * ncomps);
	}
	hio_recv_list = malloc(sizeof(hio_recv_list[0]) * ncomps);
	if (hio_recv_list == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for recv lists.",
		    sizeof(hio_recv_list[0]) * ncomps);
	}
	hio_recv_list_lock = malloc(sizeof(hio_recv_list_lock[0]) * ncomps);
	if (hio_recv_list_lock == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for recv list locks.",
		    sizeof(hio_recv_list_lock[0]) * ncomps);
	}
	hio_recv_list_cond = malloc(sizeof(hio_recv_list_cond[0]) * ncomps);
	if (hio_recv_list_cond == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for recv list condition variables.",
		    sizeof(hio_recv_list_cond[0]) * ncomps);
	}
	hio_remote_lock = malloc(sizeof(hio_remote_lock[0]) * ncomps);
	if (hio_remote_lock == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate %zu bytes of memory for remote connections locks.",
		    sizeof(hio_remote_lock[0]) * ncomps);
	}

	/*
	 * Initialize lists, their locks and theirs condition variables.
	 */
	TAILQ_INIT(&hio_free_list);
	mtx_init(&hio_free_list_lock);
	cv_init(&hio_free_list_cond);
	for (ii = 0; ii < HAST_NCOMPONENTS; ii++) {
		TAILQ_INIT(&hio_send_list[ii]);
		mtx_init(&hio_send_list_lock[ii]);
		cv_init(&hio_send_list_cond[ii]);
		TAILQ_INIT(&hio_recv_list[ii]);
		mtx_init(&hio_recv_list_lock[ii]);
		cv_init(&hio_recv_list_cond[ii]);
		rw_init(&hio_remote_lock[ii]);
	}
	TAILQ_INIT(&hio_done_list);
	mtx_init(&hio_done_list_lock);
	cv_init(&hio_done_list_cond);
	mtx_init(&hio_guard_lock);
	cv_init(&hio_guard_cond);
	mtx_init(&metadata_lock);

	/*
	 * Allocate requests pool and initialize requests.
	 */
	for (ii = 0; ii < HAST_HIO_MAX; ii++) {
		hio = malloc(sizeof(*hio));
		if (hio == NULL) {
			primary_exitx(EX_TEMPFAIL,
			    "Unable to allocate %zu bytes of memory for hio request.",
			    sizeof(*hio));
		}
		hio->hio_countdown = 0;
		hio->hio_errors = malloc(sizeof(hio->hio_errors[0]) * ncomps);
		if (hio->hio_errors == NULL) {
			primary_exitx(EX_TEMPFAIL,
			    "Unable allocate %zu bytes of memory for hio errors.",
			    sizeof(hio->hio_errors[0]) * ncomps);
		}
		hio->hio_next = malloc(sizeof(hio->hio_next[0]) * ncomps);
		if (hio->hio_next == NULL) {
			primary_exitx(EX_TEMPFAIL,
			    "Unable allocate %zu bytes of memory for hio_next field.",
			    sizeof(hio->hio_next[0]) * ncomps);
		}
		hio->hio_ggio.gctl_version = G_GATE_VERSION;
		hio->hio_ggio.gctl_data = malloc(MAXPHYS);
		if (hio->hio_ggio.gctl_data == NULL) {
			primary_exitx(EX_TEMPFAIL,
			    "Unable to allocate %zu bytes of memory for gctl_data.",
			    MAXPHYS);
		}
		hio->hio_ggio.gctl_length = MAXPHYS;
		hio->hio_ggio.gctl_error = 0;
		TAILQ_INSERT_HEAD(&hio_free_list, hio, hio_free_next);
	}

	/*
	 * Turn on signals handling.
	 */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
}

static void
init_local(struct hast_resource *res)
{
	unsigned char *buf;
	size_t mapsize;

	if (metadata_read(res, true) < 0)
		exit(EX_NOINPUT);
	mtx_init(&res->hr_amp_lock);
	if (activemap_init(&res->hr_amp, res->hr_datasize, res->hr_extentsize,
	    res->hr_local_sectorsize, res->hr_keepdirty) < 0) {
		primary_exit(EX_TEMPFAIL, "Unable to create activemap");
	}
	mtx_init(&range_lock);
	cv_init(&range_regular_cond);
	if (rangelock_init(&range_regular) < 0)
		primary_exit(EX_TEMPFAIL, "Unable to create regular range lock");
	cv_init(&range_sync_cond);
	if (rangelock_init(&range_sync) < 0)
		primary_exit(EX_TEMPFAIL, "Unable to create sync range lock");
	mapsize = activemap_ondisk_size(res->hr_amp);
	buf = calloc(1, mapsize);
	if (buf == NULL) {
		primary_exitx(EX_TEMPFAIL,
		    "Unable to allocate buffer for activemap.");
	}
	if (pread(res->hr_localfd, buf, mapsize, METADATA_SIZE) !=
	    (ssize_t)mapsize) {
		primary_exit(EX_NOINPUT, "Unable to read activemap");
	}
	activemap_copyin(res->hr_amp, buf, mapsize);
	if (res->hr_resuid != 0)
		return;
	/*
	 * We're using provider for the first time, so we have to generate
	 * resource unique identifier and initialize local and remote counts.
	 */
	arc4random_buf(&res->hr_resuid, sizeof(res->hr_resuid));
	res->hr_primary_localcnt = 1;
	res->hr_primary_remotecnt = 0;
	if (metadata_write(res) < 0)
		exit(EX_NOINPUT);
}

static bool
init_remote(struct hast_resource *res, struct proto_conn **inp,
    struct proto_conn **outp)
{
	struct proto_conn *in, *out;
	struct nv *nvout, *nvin;
	const unsigned char *token;
	unsigned char *map;
	const char *errmsg;
	int32_t extentsize;
	int64_t datasize;
	uint32_t mapsize;
	size_t size;

	assert((inp == NULL && outp == NULL) || (inp != NULL && outp != NULL));

	in = out = NULL;

	/* Prepare outgoing connection with remote node. */
	if (proto_client(res->hr_remoteaddr, &out) < 0) {
		primary_exit(EX_OSERR, "Unable to create connection to %s",
		    res->hr_remoteaddr);
	}
	/* Try to connect, but accept failure. */
	if (proto_connect(out) < 0) {
		pjdlog_errno(LOG_WARNING, "Unable to connect to %s",
		    res->hr_remoteaddr);
		goto close;
	}
	/*
	 * First handshake step.
	 * Setup outgoing connection with remote node.
	 */
	nvout = nv_alloc();
	nv_add_string(nvout, res->hr_name, "resource");
	if (nv_error(nvout) != 0) {
		pjdlog_common(LOG_WARNING, 0, nv_error(nvout),
		    "Unable to allocate header for connection with %s",
		    res->hr_remoteaddr);
		nv_free(nvout);
		goto close;
	}
	if (hast_proto_send(res, out, nvout, NULL, 0) < 0) {
		pjdlog_errno(LOG_WARNING,
		    "Unable to send handshake header to %s",
		    res->hr_remoteaddr);
		nv_free(nvout);
		goto close;
	}
	nv_free(nvout);
	if (hast_proto_recv_hdr(out, &nvin) < 0) {
		pjdlog_errno(LOG_WARNING,
		    "Unable to receive handshake header from %s",
		    res->hr_remoteaddr);
		goto close;
	}
	errmsg = nv_get_string(nvin, "errmsg");
	if (errmsg != NULL) {
		pjdlog_warning("%s", errmsg);
		nv_free(nvin);
		goto close;
	}
	token = nv_get_uint8_array(nvin, &size, "token");
	if (token == NULL) {
		pjdlog_warning("Handshake header from %s has no 'token' field.",
		    res->hr_remoteaddr);
		nv_free(nvin);
		goto close;
	}
	if (size != sizeof(res->hr_token)) {
		pjdlog_warning("Handshake header from %s contains 'token' of wrong size (got %zu, expected %zu).",
		    res->hr_remoteaddr, size, sizeof(res->hr_token));
		nv_free(nvin);
		goto close;
	}
	bcopy(token, res->hr_token, sizeof(res->hr_token));
	nv_free(nvin);

	/*
	 * Second handshake step.
	 * Setup incoming connection with remote node.
	 */
	if (proto_client(res->hr_remoteaddr, &in) < 0) {
		pjdlog_errno(LOG_WARNING, "Unable to create connection to %s",
		    res->hr_remoteaddr);
	}
	/* Try to connect, but accept failure. */
	if (proto_connect(in) < 0) {
		pjdlog_errno(LOG_WARNING, "Unable to connect to %s",
		    res->hr_remoteaddr);
		goto close;
	}
	nvout = nv_alloc();
	nv_add_string(nvout, res->hr_name, "resource");
	nv_add_uint8_array(nvout, res->hr_token, sizeof(res->hr_token),
	    "token");
	nv_add_uint64(nvout, res->hr_resuid, "resuid");
	nv_add_uint64(nvout, res->hr_primary_localcnt, "localcnt");
	nv_add_uint64(nvout, res->hr_primary_remotecnt, "remotecnt");
	if (nv_error(nvout) != 0) {
		pjdlog_common(LOG_WARNING, 0, nv_error(nvout),
		    "Unable to allocate header for connection with %s",
		    res->hr_remoteaddr);
		nv_free(nvout);
		goto close;
	}
	if (hast_proto_send(res, in, nvout, NULL, 0) < 0) {
		pjdlog_errno(LOG_WARNING,
		    "Unable to send handshake header to %s",
		    res->hr_remoteaddr);
		nv_free(nvout);
		goto close;
	}
	nv_free(nvout);
	if (hast_proto_recv_hdr(out, &nvin) < 0) {
		pjdlog_errno(LOG_WARNING,
		    "Unable to receive handshake header from %s",
		    res->hr_remoteaddr);
		goto close;
	}
	errmsg = nv_get_string(nvin, "errmsg");
	if (errmsg != NULL) {
		pjdlog_warning("%s", errmsg);
		nv_free(nvin);
		goto close;
	}
	datasize = nv_get_int64(nvin, "datasize");
	if (datasize != res->hr_datasize) {
		pjdlog_warning("Data size differs between nodes (local=%jd, remote=%jd).",
		    (intmax_t)res->hr_datasize, (intmax_t)datasize);
		nv_free(nvin);
		goto close;
	}
	extentsize = nv_get_int32(nvin, "extentsize");
	if (extentsize != res->hr_extentsize) {
		pjdlog_warning("Extent size differs between nodes (local=%zd, remote=%zd).",
		    (ssize_t)res->hr_extentsize, (ssize_t)extentsize);
		nv_free(nvin);
		goto close;
	}
	res->hr_secondary_localcnt = nv_get_uint64(nvin, "localcnt");
	res->hr_secondary_remotecnt = nv_get_uint64(nvin, "remotecnt");
	res->hr_syncsrc = nv_get_uint8(nvin, "syncsrc");
	map = NULL;
	mapsize = nv_get_uint32(nvin, "mapsize");
	if (mapsize > 0) {
		map = malloc(mapsize);
		if (map == NULL) {
			pjdlog_error("Unable to allocate memory for remote activemap (mapsize=%ju).",
			    (uintmax_t)mapsize);
			nv_free(nvin);
			goto close;
		}
		/*
		 * Remote node have some dirty extents on its own, lets
		 * download its activemap.
		 */
		if (hast_proto_recv_data(res, out, nvin, map,
		    mapsize) < 0) {
			pjdlog_errno(LOG_ERR,
			    "Unable to receive remote activemap");
			nv_free(nvin);
			free(map);
			goto close;
		}
		/*
		 * Merge local and remote bitmaps.
		 */
		activemap_merge(res->hr_amp, map, mapsize);
		free(map);
		/*
		 * Now that we merged bitmaps from both nodes, flush it to the
		 * disk before we start to synchronize.
		 */
		(void)hast_activemap_flush(res);
	}
	pjdlog_info("Connected to %s.", res->hr_remoteaddr);
	if (inp != NULL && outp != NULL) {
		*inp = in;
		*outp = out;
	} else {
		res->hr_remotein = in;
		res->hr_remoteout = out;
	}
	return (true);
close:
	proto_close(out);
	if (in != NULL)
		proto_close(in);
	return (false);
}

static void
sync_start(void)
{

	mtx_lock(&sync_lock);
	sync_inprogress = true;
	mtx_unlock(&sync_lock);
	cv_signal(&sync_cond);
}

static void
init_ggate(struct hast_resource *res)
{
	struct g_gate_ctl_create ggiocreate;
	struct g_gate_ctl_cancel ggiocancel;

	/*
	 * We communicate with ggate via /dev/ggctl. Open it.
	 */
	res->hr_ggatefd = open("/dev/" G_GATE_CTL_NAME, O_RDWR);
	if (res->hr_ggatefd < 0)
		primary_exit(EX_OSFILE, "Unable to open /dev/" G_GATE_CTL_NAME);
	/*
	 * Create provider before trying to connect, as connection failure
	 * is not critical, but may take some time.
	 */
	ggiocreate.gctl_version = G_GATE_VERSION;
	ggiocreate.gctl_mediasize = res->hr_datasize;
	ggiocreate.gctl_sectorsize = res->hr_local_sectorsize;
	ggiocreate.gctl_flags = 0;
	ggiocreate.gctl_maxcount = 128;
	ggiocreate.gctl_timeout = 0;
	ggiocreate.gctl_unit = G_GATE_NAME_GIVEN;
	snprintf(ggiocreate.gctl_name, sizeof(ggiocreate.gctl_name), "hast/%s",
	    res->hr_provname);
	bzero(ggiocreate.gctl_info, sizeof(ggiocreate.gctl_info));
	if (ioctl(res->hr_ggatefd, G_GATE_CMD_CREATE, &ggiocreate) == 0) {
		pjdlog_info("Device hast/%s created.", res->hr_provname);
		res->hr_ggateunit = ggiocreate.gctl_unit;
		return;
	}
	if (errno != EEXIST) {
		primary_exit(EX_OSERR, "Unable to create hast/%s device",
		    res->hr_provname);
	}
	pjdlog_debug(1,
	    "Device hast/%s already exists, we will try to take it over.",
	    res->hr_provname);
	/*
	 * If we received EEXIST, we assume that the process who created the
	 * provider died and didn't clean up. In that case we will start from
	 * where he left of.
	 */
	ggiocancel.gctl_version = G_GATE_VERSION;
	ggiocancel.gctl_unit = G_GATE_NAME_GIVEN;
	snprintf(ggiocancel.gctl_name, sizeof(ggiocancel.gctl_name), "hast/%s",
	    res->hr_provname);
	if (ioctl(res->hr_ggatefd, G_GATE_CMD_CANCEL, &ggiocancel) == 0) {
		pjdlog_info("Device hast/%s recovered.", res->hr_provname);
		res->hr_ggateunit = ggiocancel.gctl_unit;
		return;
	}
	primary_exit(EX_OSERR, "Unable to take over hast/%s device",
	    res->hr_provname);
}

void
hastd_primary(struct hast_resource *res)
{
	pthread_t td;
	pid_t pid;
	int error;

	gres = res;

	/*
	 * Create communication channel between parent and child.
	 */
	if (proto_client("socketpair://", &res->hr_ctrl) < 0) {
		KEEP_ERRNO((void)pidfile_remove(pfh));
		primary_exit(EX_OSERR,
		    "Unable to create control sockets between parent and child");
	}

	pid = fork();
	if (pid < 0) {
		KEEP_ERRNO((void)pidfile_remove(pfh));
		primary_exit(EX_OSERR, "Unable to fork");
	}

	if (pid > 0) {
		/* This is parent. */
		res->hr_workerpid = pid;
		return;
	}
	(void)pidfile_close(pfh);

	setproctitle("%s (primary)", res->hr_name);

	init_local(res);
	if (init_remote(res, NULL, NULL))
		sync_start();
	init_ggate(res);
	init_environment(res);
	error = pthread_create(&td, NULL, ggate_recv_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, local_send_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, remote_send_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, remote_recv_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, ggate_send_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, sync_thread, res);
	assert(error == 0);
	error = pthread_create(&td, NULL, ctrl_thread, res);
	assert(error == 0);
	(void)guard_thread(res);
}

static void
reqlog(int loglevel, int debuglevel, struct g_gate_ctl_io *ggio, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if ((size_t)len < sizeof(msg)) {
		switch (ggio->gctl_cmd) {
		case BIO_READ:
			(void)snprintf(msg + len, sizeof(msg) - len,
			    "READ(%ju, %ju).", (uintmax_t)ggio->gctl_offset,
			    (uintmax_t)ggio->gctl_length);
			break;
		case BIO_DELETE:
			(void)snprintf(msg + len, sizeof(msg) - len,
			    "DELETE(%ju, %ju).", (uintmax_t)ggio->gctl_offset,
			    (uintmax_t)ggio->gctl_length);
			break;
		case BIO_FLUSH:
			(void)snprintf(msg + len, sizeof(msg) - len, "FLUSH.");
			break;
		case BIO_WRITE:
			(void)snprintf(msg + len, sizeof(msg) - len,
			    "WRITE(%ju, %ju).", (uintmax_t)ggio->gctl_offset,
			    (uintmax_t)ggio->gctl_length);
			break;
		default:
			(void)snprintf(msg + len, sizeof(msg) - len,
			    "UNKNOWN(%u).", (unsigned int)ggio->gctl_cmd);
			break;
		}
	}
	pjdlog_common(loglevel, debuglevel, -1, "%s", msg);
}

static void
remote_close(struct hast_resource *res, int ncomp)
{

	rw_wlock(&hio_remote_lock[ncomp]);
	/*
	 * A race is possible between dropping rlock and acquiring wlock -
	 * another thread can close connection in-between.
	 */
	if (!ISCONNECTED(res, ncomp)) {
		assert(res->hr_remotein == NULL);
		assert(res->hr_remoteout == NULL);
		rw_unlock(&hio_remote_lock[ncomp]);
		return;
	}

	assert(res->hr_remotein != NULL);
	assert(res->hr_remoteout != NULL);

	pjdlog_debug(2, "Closing old incoming connection to %s.",
	    res->hr_remoteaddr);
	proto_close(res->hr_remotein);
	res->hr_remotein = NULL;
	pjdlog_debug(2, "Closing old outgoing connection to %s.",
	    res->hr_remoteaddr);
	proto_close(res->hr_remoteout);
	res->hr_remoteout = NULL;

	rw_unlock(&hio_remote_lock[ncomp]);

	/*
	 * Stop synchronization if in-progress.
	 */
	mtx_lock(&sync_lock);
	if (sync_inprogress)
		sync_inprogress = false;
	mtx_unlock(&sync_lock);

	/*
	 * Wake up guard thread, so it can immediately start reconnect.
	 */
	mtx_lock(&hio_guard_lock);
	cv_signal(&hio_guard_cond);
	mtx_unlock(&hio_guard_lock);
}

/*
 * Thread receives ggate I/O requests from the kernel and passes them to
 * appropriate threads:
 * WRITE - always goes to both local_send and remote_send threads
 * READ (when the block is up-to-date on local component) -
 *	only local_send thread
 * READ (when the block isn't up-to-date on local component) -
 *	only remote_send thread
 * DELETE - always goes to both local_send and remote_send threads
 * FLUSH - always goes to both local_send and remote_send threads
 */
static void *
ggate_recv_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct g_gate_ctl_io *ggio;
	struct hio *hio;
	unsigned int ii, ncomp, ncomps;
	int error;

	ncomps = HAST_NCOMPONENTS;

	for (;;) {
		pjdlog_debug(2, "ggate_recv: Taking free request.");
		QUEUE_TAKE2(hio, free);
		pjdlog_debug(2, "ggate_recv: (%p) Got free request.", hio);
		ggio = &hio->hio_ggio;
		ggio->gctl_unit = res->hr_ggateunit;
		ggio->gctl_length = MAXPHYS;
		ggio->gctl_error = 0;
		pjdlog_debug(2,
		    "ggate_recv: (%p) Waiting for request from the kernel.",
		    hio);
		if (ioctl(res->hr_ggatefd, G_GATE_CMD_START, ggio) < 0) {
			if (sigexit_received)
				pthread_exit(NULL);
			primary_exit(EX_OSERR, "G_GATE_CMD_START failed");
		}
		error = ggio->gctl_error;
		switch (error) {
		case 0:
			break;
		case ECANCELED:
			/* Exit gracefully. */
			if (!sigexit_received) {
				pjdlog_debug(2,
				    "ggate_recv: (%p) Received cancel from the kernel.",
				    hio);
				pjdlog_info("Received cancel from the kernel, exiting.");
			}
			pthread_exit(NULL);
		case ENOMEM:
			/*
			 * Buffer too small? Impossible, we allocate MAXPHYS
			 * bytes - request can't be bigger than that.
			 */
			/* FALLTHROUGH */
		case ENXIO:
		default:
			primary_exitx(EX_OSERR, "G_GATE_CMD_START failed: %s.",
			    strerror(error));
		}
		for (ii = 0; ii < ncomps; ii++)
			hio->hio_errors[ii] = EINVAL;
		reqlog(LOG_DEBUG, 2, ggio,
		    "ggate_recv: (%p) Request received from the kernel: ",
		    hio);
		/*
		 * Inform all components about new write request.
		 * For read request prefer local component unless the given
		 * range is out-of-date, then use remote component.
		 */
		switch (ggio->gctl_cmd) {
		case BIO_READ:
			pjdlog_debug(2,
			    "ggate_recv: (%p) Moving request to the send queue.",
			    hio);
			refcount_init(&hio->hio_countdown, 1);
			mtx_lock(&metadata_lock);
			if (res->hr_syncsrc == HAST_SYNCSRC_UNDEF ||
			    res->hr_syncsrc == HAST_SYNCSRC_PRIMARY) {
				/*
				 * This range is up-to-date on local component,
				 * so handle request locally.
				 */
				 /* Local component is 0 for now. */
				ncomp = 0;
			} else /* if (res->hr_syncsrc ==
			    HAST_SYNCSRC_SECONDARY) */ {
				assert(res->hr_syncsrc ==
				    HAST_SYNCSRC_SECONDARY);
				/*
				 * This range is out-of-date on local component,
				 * so send request to the remote node.
				 */
				 /* Remote component is 1 for now. */
				ncomp = 1;
			}
			mtx_unlock(&metadata_lock);
			QUEUE_INSERT1(hio, send, ncomp);
			break;
		case BIO_WRITE:
			for (;;) {
				mtx_lock(&range_lock);
				if (rangelock_islocked(range_sync,
				    ggio->gctl_offset, ggio->gctl_length)) {
					pjdlog_debug(2,
					    "regular: Range offset=%jd length=%zu locked.",
					    (intmax_t)ggio->gctl_offset,
					    (size_t)ggio->gctl_length);
					range_regular_wait = true;
					cv_wait(&range_regular_cond, &range_lock);
					range_regular_wait = false;
					mtx_unlock(&range_lock);
					continue;
				}
				if (rangelock_add(range_regular,
				    ggio->gctl_offset, ggio->gctl_length) < 0) {
					mtx_unlock(&range_lock);
					pjdlog_debug(2,
					    "regular: Range offset=%jd length=%zu is already locked, waiting.",
					    (intmax_t)ggio->gctl_offset,
					    (size_t)ggio->gctl_length);
					sleep(1);
					continue;
				}
				mtx_unlock(&range_lock);
				break;
			}
			mtx_lock(&res->hr_amp_lock);
			if (activemap_write_start(res->hr_amp,
			    ggio->gctl_offset, ggio->gctl_length)) {
				(void)hast_activemap_flush(res);
			}
			mtx_unlock(&res->hr_amp_lock);
			/* FALLTHROUGH */
		case BIO_DELETE:
		case BIO_FLUSH:
			pjdlog_debug(2,
			    "ggate_recv: (%p) Moving request to the send queues.",
			    hio);
			refcount_init(&hio->hio_countdown, ncomps);
			for (ii = 0; ii < ncomps; ii++)
				QUEUE_INSERT1(hio, send, ii);
			break;
		}
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Thread reads from or writes to local component.
 * If local read fails, it redirects it to remote_send thread.
 */
static void *
local_send_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct g_gate_ctl_io *ggio;
	struct hio *hio;
	unsigned int ncomp, rncomp;
	ssize_t ret;

	/* Local component is 0 for now. */
	ncomp = 0;
	/* Remote component is 1 for now. */
	rncomp = 1;

	for (;;) {
		pjdlog_debug(2, "local_send: Taking request.");
		QUEUE_TAKE1(hio, send, ncomp);
		pjdlog_debug(2, "local_send: (%p) Got request.", hio);
		ggio = &hio->hio_ggio;
		switch (ggio->gctl_cmd) {
		case BIO_READ:
			ret = pread(res->hr_localfd, ggio->gctl_data,
			    ggio->gctl_length,
			    ggio->gctl_offset + res->hr_localoff);
			if (ret == ggio->gctl_length)
				hio->hio_errors[ncomp] = 0;
			else {
				/*
				 * If READ failed, try to read from remote node.
				 */
				QUEUE_INSERT1(hio, send, rncomp);
				continue;
			}
			break;
		case BIO_WRITE:
			ret = pwrite(res->hr_localfd, ggio->gctl_data,
			    ggio->gctl_length,
			    ggio->gctl_offset + res->hr_localoff);
			if (ret < 0)
				hio->hio_errors[ncomp] = errno;
			else if (ret != ggio->gctl_length)
				hio->hio_errors[ncomp] = EIO;
			else
				hio->hio_errors[ncomp] = 0;
			break;
		case BIO_DELETE:
			ret = g_delete(res->hr_localfd,
			    ggio->gctl_offset + res->hr_localoff,
			    ggio->gctl_length);
			if (ret < 0)
				hio->hio_errors[ncomp] = errno;
			else
				hio->hio_errors[ncomp] = 0;
			break;
		case BIO_FLUSH:
			ret = g_flush(res->hr_localfd);
			if (ret < 0)
				hio->hio_errors[ncomp] = errno;
			else
				hio->hio_errors[ncomp] = 0;
			break;
		}
		if (refcount_release(&hio->hio_countdown)) {
			if (ISSYNCREQ(hio)) {
				mtx_lock(&sync_lock);
				SYNCREQDONE(hio);
				mtx_unlock(&sync_lock);
				cv_signal(&sync_cond);
			} else {
				pjdlog_debug(2,
				    "local_send: (%p) Moving request to the done queue.",
				    hio);
				QUEUE_INSERT2(hio, done);
			}
		}
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Thread sends request to secondary node.
 */
static void *
remote_send_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct g_gate_ctl_io *ggio;
	struct hio *hio;
	struct nv *nv;
	unsigned int ncomp;
	bool wakeup;
	uint64_t offset, length;
	uint8_t cmd;
	void *data;

	/* Remote component is 1 for now. */
	ncomp = 1;

	for (;;) {
		pjdlog_debug(2, "remote_send: Taking request.");
		QUEUE_TAKE1(hio, send, ncomp);
		pjdlog_debug(2, "remote_send: (%p) Got request.", hio);
		ggio = &hio->hio_ggio;
		switch (ggio->gctl_cmd) {
		case BIO_READ:
			cmd = HIO_READ;
			data = NULL;
			offset = ggio->gctl_offset;
			length = ggio->gctl_length;
			break;
		case BIO_WRITE:
			cmd = HIO_WRITE;
			data = ggio->gctl_data;
			offset = ggio->gctl_offset;
			length = ggio->gctl_length;
			break;
		case BIO_DELETE:
			cmd = HIO_DELETE;
			data = NULL;
			offset = ggio->gctl_offset;
			length = ggio->gctl_length;
			break;
		case BIO_FLUSH:
			cmd = HIO_FLUSH;
			data = NULL;
			offset = 0;
			length = 0;
			break;
		default:
			assert(!"invalid condition");
			abort();
		}
		nv = nv_alloc();
		nv_add_uint8(nv, cmd, "cmd");
		nv_add_uint64(nv, (uint64_t)ggio->gctl_seq, "seq");
		nv_add_uint64(nv, offset, "offset");
		nv_add_uint64(nv, length, "length");
		if (nv_error(nv) != 0) {
			hio->hio_errors[ncomp] = nv_error(nv);
			pjdlog_debug(2,
			    "remote_send: (%p) Unable to prepare header to send.",
			    hio);
			reqlog(LOG_ERR, 0, ggio,
			    "Unable to prepare header to send (%s): ",
			    strerror(nv_error(nv)));
			/* Move failed request immediately to the done queue. */
			goto done_queue;
		}
		pjdlog_debug(2,
		    "remote_send: (%p) Moving request to the recv queue.",
		    hio);
		/*
		 * Protect connection from disappearing.
		 */
		rw_rlock(&hio_remote_lock[ncomp]);
		if (!ISCONNECTED(res, ncomp)) {
			rw_unlock(&hio_remote_lock[ncomp]);
			hio->hio_errors[ncomp] = ENOTCONN;
			goto done_queue;
		}
		/*
		 * Move the request to recv queue before sending it, because
		 * in different order we can get reply before we move request
		 * to recv queue.
		 */
		mtx_lock(&hio_recv_list_lock[ncomp]);
		wakeup = TAILQ_EMPTY(&hio_recv_list[ncomp]);
		TAILQ_INSERT_TAIL(&hio_recv_list[ncomp], hio, hio_next[ncomp]);
		mtx_unlock(&hio_recv_list_lock[ncomp]);
		if (hast_proto_send(res, res->hr_remoteout, nv, data,
		    data != NULL ? length : 0) < 0) {
			hio->hio_errors[ncomp] = errno;
			rw_unlock(&hio_remote_lock[ncomp]);
			remote_close(res, ncomp);
			pjdlog_debug(2,
			    "remote_send: (%p) Unable to send request.", hio);
			reqlog(LOG_ERR, 0, ggio,
			    "Unable to send request (%s): ",
			    strerror(hio->hio_errors[ncomp]));
			/*
			 * Take request back from the receive queue and move
			 * it immediately to the done queue.
			 */
			mtx_lock(&hio_recv_list_lock[ncomp]);
			TAILQ_REMOVE(&hio_recv_list[ncomp], hio, hio_next[ncomp]);
			mtx_unlock(&hio_recv_list_lock[ncomp]);
			goto done_queue;
		}
		rw_unlock(&hio_remote_lock[ncomp]);
		nv_free(nv);
		if (wakeup)
			cv_signal(&hio_recv_list_cond[ncomp]);
		continue;
done_queue:
		nv_free(nv);
		if (ISSYNCREQ(hio)) {
			if (!refcount_release(&hio->hio_countdown))
				continue;
			mtx_lock(&sync_lock);
			SYNCREQDONE(hio);
			mtx_unlock(&sync_lock);
			cv_signal(&sync_cond);
			continue;
		}
		if (ggio->gctl_cmd == BIO_WRITE) {
			mtx_lock(&res->hr_amp_lock);
			if (activemap_need_sync(res->hr_amp, ggio->gctl_offset,
			    ggio->gctl_length)) {
				(void)hast_activemap_flush(res);
			}
			mtx_unlock(&res->hr_amp_lock);
		}
		if (!refcount_release(&hio->hio_countdown))
			continue;
		pjdlog_debug(2,
		    "remote_send: (%p) Moving request to the done queue.",
		    hio);
		QUEUE_INSERT2(hio, done);
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Thread receives answer from secondary node and passes it to ggate_send
 * thread.
 */
static void *
remote_recv_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct g_gate_ctl_io *ggio;
	struct hio *hio;
	struct nv *nv;
	unsigned int ncomp;
	uint64_t seq;
	int error;

	/* Remote component is 1 for now. */
	ncomp = 1;

	for (;;) {
		/* Wait until there is anything to receive. */
		mtx_lock(&hio_recv_list_lock[ncomp]);
		while (TAILQ_EMPTY(&hio_recv_list[ncomp])) {
			pjdlog_debug(2, "remote_recv: No requests, waiting.");
			cv_wait(&hio_recv_list_cond[ncomp],
			    &hio_recv_list_lock[ncomp]);
		}
		mtx_unlock(&hio_recv_list_lock[ncomp]);
		rw_rlock(&hio_remote_lock[ncomp]);
		if (!ISCONNECTED(res, ncomp)) {
			rw_unlock(&hio_remote_lock[ncomp]);
			/*
			 * Connection is dead, so move all pending requests to
			 * the done queue (one-by-one).
			 */
			mtx_lock(&hio_recv_list_lock[ncomp]);
			hio = TAILQ_FIRST(&hio_recv_list[ncomp]);
			assert(hio != NULL);
			TAILQ_REMOVE(&hio_recv_list[ncomp], hio,
			    hio_next[ncomp]);
			mtx_unlock(&hio_recv_list_lock[ncomp]);
			goto done_queue;
		}
		if (hast_proto_recv_hdr(res->hr_remotein, &nv) < 0) {
			pjdlog_errno(LOG_ERR,
			    "Unable to receive reply header");
			rw_unlock(&hio_remote_lock[ncomp]);
			remote_close(res, ncomp);
			continue;
		}
		rw_unlock(&hio_remote_lock[ncomp]);
		seq = nv_get_uint64(nv, "seq");
		if (seq == 0) {
			pjdlog_error("Header contains no 'seq' field.");
			nv_free(nv);
			continue;
		}
		mtx_lock(&hio_recv_list_lock[ncomp]);
		TAILQ_FOREACH(hio, &hio_recv_list[ncomp], hio_next[ncomp]) {
			if (hio->hio_ggio.gctl_seq == seq) {
				TAILQ_REMOVE(&hio_recv_list[ncomp], hio,
				    hio_next[ncomp]);
				break;
			}
		}
		mtx_unlock(&hio_recv_list_lock[ncomp]);
		if (hio == NULL) {
			pjdlog_error("Found no request matching received 'seq' field (%ju).",
			    (uintmax_t)seq);
			nv_free(nv);
			continue;
		}
		error = nv_get_int16(nv, "error");
		if (error != 0) {
			/* Request failed on remote side. */
			hio->hio_errors[ncomp] = 0;
			nv_free(nv);
			goto done_queue;
		}
		ggio = &hio->hio_ggio;
		switch (ggio->gctl_cmd) {
		case BIO_READ:
			rw_rlock(&hio_remote_lock[ncomp]);
			if (!ISCONNECTED(res, ncomp)) {
				rw_unlock(&hio_remote_lock[ncomp]);
				nv_free(nv);
				goto done_queue;
			}
			if (hast_proto_recv_data(res, res->hr_remotein, nv,
			    ggio->gctl_data, ggio->gctl_length) < 0) {
				hio->hio_errors[ncomp] = errno;
				pjdlog_errno(LOG_ERR,
				    "Unable to receive reply data");
				rw_unlock(&hio_remote_lock[ncomp]);
				nv_free(nv);
				remote_close(res, ncomp);
				goto done_queue;
			}
			rw_unlock(&hio_remote_lock[ncomp]);
			break;
		case BIO_WRITE:
		case BIO_DELETE:
		case BIO_FLUSH:
			break;
		default:
			assert(!"invalid condition");
			abort();
		}
		hio->hio_errors[ncomp] = 0;
		nv_free(nv);
done_queue:
		if (refcount_release(&hio->hio_countdown)) {
			if (ISSYNCREQ(hio)) {
				mtx_lock(&sync_lock);
				SYNCREQDONE(hio);
				mtx_unlock(&sync_lock);
				cv_signal(&sync_cond);
			} else {
				pjdlog_debug(2,
				    "remote_recv: (%p) Moving request to the done queue.",
				    hio);
				QUEUE_INSERT2(hio, done);
			}
		}
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Thread sends answer to the kernel.
 */
static void *
ggate_send_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct g_gate_ctl_io *ggio;
	struct hio *hio;
	unsigned int ii, ncomp, ncomps;

	ncomps = HAST_NCOMPONENTS;

	for (;;) {
		pjdlog_debug(2, "ggate_send: Taking request.");
		QUEUE_TAKE2(hio, done);
		pjdlog_debug(2, "ggate_send: (%p) Got request.", hio);
		ggio = &hio->hio_ggio;
		for (ii = 0; ii < ncomps; ii++) {
			if (hio->hio_errors[ii] == 0) {
				/*
				 * One successful request is enough to declare
				 * success.
				 */
				ggio->gctl_error = 0;
				break;
			}
		}
		if (ii == ncomps) {
			/*
			 * None of the requests were successful.
			 * Use first error.
			 */
			ggio->gctl_error = hio->hio_errors[0];
		}
		if (ggio->gctl_error == 0 && ggio->gctl_cmd == BIO_WRITE) {
			mtx_lock(&res->hr_amp_lock);
			activemap_write_complete(res->hr_amp,
			    ggio->gctl_offset, ggio->gctl_length);
			mtx_unlock(&res->hr_amp_lock);
		}
		if (ggio->gctl_cmd == BIO_WRITE) {
			/*
			 * Unlock range we locked.
			 */
			mtx_lock(&range_lock);
			rangelock_del(range_regular, ggio->gctl_offset,
			    ggio->gctl_length);
			if (range_sync_wait)
				cv_signal(&range_sync_cond);
			mtx_unlock(&range_lock);
			/*
			 * Bump local count if this is first write after
			 * connection failure with remote node.
			 */
			ncomp = 1;
			rw_rlock(&hio_remote_lock[ncomp]);
			if (!ISCONNECTED(res, ncomp)) {
				mtx_lock(&metadata_lock);
				if (res->hr_primary_localcnt ==
				    res->hr_secondary_remotecnt) {
					res->hr_primary_localcnt++;
					pjdlog_debug(1,
					    "Increasing localcnt to %ju.",
					    (uintmax_t)res->hr_primary_localcnt);
					(void)metadata_write(res);
				}
				mtx_unlock(&metadata_lock);
			}
			rw_unlock(&hio_remote_lock[ncomp]);
		}
		if (ioctl(res->hr_ggatefd, G_GATE_CMD_DONE, ggio) < 0)
			primary_exit(EX_OSERR, "G_GATE_CMD_DONE failed");
		pjdlog_debug(2,
		    "ggate_send: (%p) Moving request to the free queue.", hio);
		QUEUE_INSERT2(hio, free);
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Thread synchronize local and remote components.
 */
static void *
sync_thread(void *arg __unused)
{
	struct hast_resource *res = arg;
	struct hio *hio;
	struct g_gate_ctl_io *ggio;
	unsigned int ii, ncomp, ncomps;
	off_t offset, length, synced;
	bool dorewind;
	int syncext;

	ncomps = HAST_NCOMPONENTS;
	dorewind = true;
	synced = 0;

	for (;;) {
		mtx_lock(&sync_lock);
		while (!sync_inprogress) {
			dorewind = true;
			synced = 0;
			cv_wait(&sync_cond, &sync_lock);
		}
		mtx_unlock(&sync_lock);
		/*
		 * Obtain offset at which we should synchronize.
		 * Rewind synchronization if needed.
		 */
		mtx_lock(&res->hr_amp_lock);
		if (dorewind)
			activemap_sync_rewind(res->hr_amp);
		offset = activemap_sync_offset(res->hr_amp, &length, &syncext);
		if (syncext != -1) {
			/*
			 * We synchronized entire syncext extent, we can mark
			 * it as clean now.
			 */
			if (activemap_extent_complete(res->hr_amp, syncext))
				(void)hast_activemap_flush(res);
		}
		mtx_unlock(&res->hr_amp_lock);
		if (dorewind) {
			dorewind = false;
			if (offset < 0)
				pjdlog_info("Nodes are in sync.");
			else {
				pjdlog_info("Synchronization started. %ju bytes to go.",
				    (uintmax_t)(res->hr_extentsize *
				    activemap_ndirty(res->hr_amp)));
			}
		}
		if (offset < 0) {
			mtx_lock(&sync_lock);
			sync_inprogress = false;
			mtx_unlock(&sync_lock);
			pjdlog_debug(1, "Nothing to synchronize.");
			/*
			 * Synchronization complete, make both localcnt and
			 * remotecnt equal.
			 */
			ncomp = 1;
			rw_rlock(&hio_remote_lock[ncomp]);
			if (ISCONNECTED(res, ncomp)) {
				if (synced > 0) {
					pjdlog_info("Synchronization complete. "
					    "%jd bytes synchronized.",
					    (intmax_t)synced);
				}
				mtx_lock(&metadata_lock);
				res->hr_syncsrc = HAST_SYNCSRC_UNDEF;
				res->hr_primary_localcnt =
				    res->hr_secondary_localcnt;
				res->hr_primary_remotecnt =
				    res->hr_secondary_remotecnt;
				pjdlog_debug(1,
				    "Setting localcnt to %ju and remotecnt to %ju.",
				    (uintmax_t)res->hr_primary_localcnt,
				    (uintmax_t)res->hr_secondary_localcnt);
				(void)metadata_write(res);
				mtx_unlock(&metadata_lock);
			} else if (synced > 0) {
				pjdlog_info("Synchronization interrupted. "
				    "%jd bytes synchronized so far.",
				    (intmax_t)synced);
			}
			rw_unlock(&hio_remote_lock[ncomp]);
			continue;
		}
		pjdlog_debug(2, "sync: Taking free request.");
		QUEUE_TAKE2(hio, free);
		pjdlog_debug(2, "sync: (%p) Got free request.", hio);
		/*
		 * Lock the range we are going to synchronize. We don't want
		 * race where someone writes between our read and write.
		 */
		for (;;) {
			mtx_lock(&range_lock);
			if (rangelock_islocked(range_regular, offset, length)) {
				pjdlog_debug(2,
				    "sync: Range offset=%jd length=%jd locked.",
				    (intmax_t)offset, (intmax_t)length);
				range_sync_wait = true;
				cv_wait(&range_sync_cond, &range_lock);
				range_sync_wait = false;
				mtx_unlock(&range_lock);
				continue;
			}
			if (rangelock_add(range_sync, offset, length) < 0) {
				mtx_unlock(&range_lock);
				pjdlog_debug(2,
				    "sync: Range offset=%jd length=%jd is already locked, waiting.",
				    (intmax_t)offset, (intmax_t)length);
				sleep(1);
				continue;
			}
			mtx_unlock(&range_lock);
			break;
		}
		/*
		 * First read the data from synchronization source.
		 */
		SYNCREQ(hio);
		ggio = &hio->hio_ggio;
		ggio->gctl_cmd = BIO_READ;
		ggio->gctl_offset = offset;
		ggio->gctl_length = length;
		ggio->gctl_error = 0;
		for (ii = 0; ii < ncomps; ii++)
			hio->hio_errors[ii] = EINVAL;
		reqlog(LOG_DEBUG, 2, ggio, "sync: (%p) Sending sync request: ",
		    hio);
		pjdlog_debug(2, "sync: (%p) Moving request to the send queue.",
		    hio);
		mtx_lock(&metadata_lock);
		if (res->hr_syncsrc == HAST_SYNCSRC_PRIMARY) {
			/*
			 * This range is up-to-date on local component,
			 * so handle request locally.
			 */
			 /* Local component is 0 for now. */
			ncomp = 0;
		} else /* if (res->hr_syncsrc == HAST_SYNCSRC_SECONDARY) */ {
			assert(res->hr_syncsrc == HAST_SYNCSRC_SECONDARY);
			/*
			 * This range is out-of-date on local component,
			 * so send request to the remote node.
			 */
			 /* Remote component is 1 for now. */
			ncomp = 1;
		}
		mtx_unlock(&metadata_lock);
		refcount_init(&hio->hio_countdown, 1);
		QUEUE_INSERT1(hio, send, ncomp);

		/*
		 * Let's wait for READ to finish.
		 */
		mtx_lock(&sync_lock);
		while (!ISSYNCREQDONE(hio))
			cv_wait(&sync_cond, &sync_lock);
		mtx_unlock(&sync_lock);

		if (hio->hio_errors[ncomp] != 0) {
			pjdlog_error("Unable to read synchronization data: %s.",
			    strerror(hio->hio_errors[ncomp]));
			goto free_queue;
		}

		/*
		 * We read the data from synchronization source, now write it
		 * to synchronization target.
		 */
		SYNCREQ(hio);
		ggio->gctl_cmd = BIO_WRITE;
		for (ii = 0; ii < ncomps; ii++)
			hio->hio_errors[ii] = EINVAL;
		reqlog(LOG_DEBUG, 2, ggio, "sync: (%p) Sending sync request: ",
		    hio);
		pjdlog_debug(2, "sync: (%p) Moving request to the send queue.",
		    hio);
		mtx_lock(&metadata_lock);
		if (res->hr_syncsrc == HAST_SYNCSRC_PRIMARY) {
			/*
			 * This range is up-to-date on local component,
			 * so we update remote component.
			 */
			 /* Remote component is 1 for now. */
			ncomp = 1;
		} else /* if (res->hr_syncsrc == HAST_SYNCSRC_SECONDARY) */ {
			assert(res->hr_syncsrc == HAST_SYNCSRC_SECONDARY);
			/*
			 * This range is out-of-date on local component,
			 * so we update it.
			 */
			 /* Local component is 0 for now. */
			ncomp = 0;
		}
		mtx_unlock(&metadata_lock);

		pjdlog_debug(2, "sync: (%p) Moving request to the send queues.",
		    hio);
		refcount_init(&hio->hio_countdown, 1);
		QUEUE_INSERT1(hio, send, ncomp);

		/*
		 * Let's wait for WRITE to finish.
		 */
		mtx_lock(&sync_lock);
		while (!ISSYNCREQDONE(hio))
			cv_wait(&sync_cond, &sync_lock);
		mtx_unlock(&sync_lock);

		if (hio->hio_errors[ncomp] != 0) {
			pjdlog_error("Unable to write synchronization data: %s.",
			    strerror(hio->hio_errors[ncomp]));
			goto free_queue;
		}
free_queue:
		mtx_lock(&range_lock);
		rangelock_del(range_sync, offset, length);
		if (range_regular_wait)
			cv_signal(&range_regular_cond);
		mtx_unlock(&range_lock);

		synced += length;

		pjdlog_debug(2, "sync: (%p) Moving request to the free queue.",
		    hio);
		QUEUE_INSERT2(hio, free);
	}
	/* NOTREACHED */
	return (NULL);
}

static void
sighandler(int sig)
{
	bool unlock;

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		sigexit_received = true;
		break;
	default:
		assert(!"invalid condition");
	}
	/*
	 * XXX: Racy, but if we cannot obtain hio_guard_lock here, we don't
	 * want to risk deadlock.
	 */
	unlock = mtx_trylock(&hio_guard_lock);
	cv_signal(&hio_guard_cond);
	if (unlock)
		mtx_unlock(&hio_guard_lock);
}

/*
 * Thread guards remote connections and reconnects when needed, handles
 * signals, etc.
 */
static void *
guard_thread(void *arg)
{
	struct hast_resource *res = arg;
	struct proto_conn *in, *out;
	unsigned int ii, ncomps;
	int timeout;

	ncomps = HAST_NCOMPONENTS;
	/* The is only one remote component for now. */
#define	ISREMOTE(no)	((no) == 1)

	for (;;) {
		if (sigexit_received) {
			primary_exitx(EX_OK,
			    "Termination signal received, exiting.");
		}
		/*
		 * If all the connection will be fine, we will sleep until
		 * someone wakes us up.
		 * If any of the connections will be broken and we won't be
		 * able to connect, we will sleep only for RECONNECT_SLEEP
		 * seconds so we can retry soon.
		 */
		timeout = 0;
		pjdlog_debug(2, "remote_guard: Checking connections.");
		mtx_lock(&hio_guard_lock);
		for (ii = 0; ii < ncomps; ii++) {
			if (!ISREMOTE(ii))
				continue;
			rw_rlock(&hio_remote_lock[ii]);
			if (ISCONNECTED(res, ii)) {
				assert(res->hr_remotein != NULL);
				assert(res->hr_remoteout != NULL);
				rw_unlock(&hio_remote_lock[ii]);
				pjdlog_debug(2,
				    "remote_guard: Connection to %s is ok.",
				    res->hr_remoteaddr);
			} else {
				assert(res->hr_remotein == NULL);
				assert(res->hr_remoteout == NULL);
				/*
				 * Upgrade the lock. It doesn't have to be
				 * atomic as no other thread can change
				 * connection status from disconnected to
				 * connected.
				 */
				rw_unlock(&hio_remote_lock[ii]);
				pjdlog_debug(2,
				    "remote_guard: Reconnecting to %s.",
				    res->hr_remoteaddr);
				in = out = NULL;
				if (init_remote(res, &in, &out)) {
					rw_wlock(&hio_remote_lock[ii]);
					assert(res->hr_remotein == NULL);
					assert(res->hr_remoteout == NULL);
					assert(in != NULL && out != NULL);
					res->hr_remotein = in;
					res->hr_remoteout = out;
					rw_unlock(&hio_remote_lock[ii]);
					pjdlog_info("Successfully reconnected to %s.",
					    res->hr_remoteaddr);
					sync_start();
				} else {
					/* Both connections should be NULL. */
					assert(res->hr_remotein == NULL);
					assert(res->hr_remoteout == NULL);
					assert(in == NULL && out == NULL);
					pjdlog_debug(2,
					    "remote_guard: Reconnect to %s failed.",
					    res->hr_remoteaddr);
					timeout = RECONNECT_SLEEP;
				}
			}
		}
		(void)cv_timedwait(&hio_guard_cond, &hio_guard_lock, timeout);
		mtx_unlock(&hio_guard_lock);
	}
#undef	ISREMOTE
	/* NOTREACHED */
	return (NULL);
}
