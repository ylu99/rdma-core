/*
 * Copyright (c) 2018 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#define _GNU_SOURCE
#include <config.h>

#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include <rdma/rsocket.h>
#include "indexer.h"


static struct index_map ep_idm;
static pthread_mutex_t ep_mut = PTHREAD_MUTEX_INITIALIZER;


struct repoll {
	int		epfd;
	int		size;
	int		nfds;
	struct pollfd	*fds;
	epoll_data_t	*data;
	int		pos;	/* used for fairness */
};


static int repoll_map_events(int events)
{
	int set = 0;

	if (events & EPOLLIN)
		set |= POLLIN;
	if (events & EPOLLOUT)
		set |= POLLOUT;
	if (events & EPOLLRDHUP)
		set |= POLLRDHUP;
	if (events & EPOLLPRI)
		set |= POLLPRI;

	return set;
}

static int repoll_unmap_events(int events)
{
	int set = 0;

	if (events & POLLIN)
		set |= EPOLLIN;
	if (events & POLLOUT)
		set |= EPOLLOUT;
	if (events & POLLHUP)
		set |= EPOLLHUP;
	if (events & POLLERR)
		set |= EPOLLERR;
	if (events & POLLRDHUP)
		set |= EPOLLRDHUP;
	if (events & POLLPRI)
		set |= EPOLLPRI;

	return set;
}

static int rep_insert(struct repoll *rep)
{
	int ret;

	pthread_mutex_lock(&ep_mut);
	ret = idm_set(&ep_idm, rep->epfd);
	pthread_mutex_unlock(&ep_mut);
	return ret;
}

static void rep_remove(struct repoll *rep)
{
	pthread_mutex_lock(&ep_mut);
	idm_clear(&ep_idm, rep->epfd);
	pthread_mutex_unlock(&ep_mut);
}

int repoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	struct repoll *rep;
	int i, ret;
	int found = 0;

	rep = idm_at(&ep_idm, epfd);
	if (!rep)
		return ERR(EBADF);

	ret = rpoll(rep->fds, rep->nfds, timeout);
	if (ret == -1)
		return -1;
	else if (ret == 0)
		return 0;

	for (i = ep->pos; i < ep->nfds && found < maxevents; i++) {
		if (rep->fds[i].revents) {
			events[found].data = rep->data[i];
			events[found++].events =
					repoll_unmap_events(rep->fds[i].revents);
			rep->pos = i;
		}
	}

	for (i = 0; i < rep->pos && found < maxevents; i++) {
		if (rep->fds[i].revents) {
			events[found].data = rep->data[i];
			events[found++].events =
					repoll_unmap_events(rep->fds[i].revents);
			rep->pos = i;
		}
	}
	return found;
}

static int repoll_add(struct repoll *rep, int fd, struct epoll_event *event)
{
	struct pollfd *fds;
	void *contexts;

	if (rep->nfds == ep->size) {
		fds = calloc(rep->size + 64,
			     sizeof(*rep->fds) + sizeof(*rep->data));
		if (!fds)
			return ERR(ENOMEM);

		rep->size += 64;
		contexts = fds + rep->size;

		memcpy(fds, rep->fds, rep->nfds * sizeof(*rep->fds));
		memcpy(contexts, rep->data, rep->nfds * sizeof(*rep->data));
		free(rep->fds);
		rep->fds = fds;
		rep->data = contexts;
	}

	rep->fds[rep->nfds].fd = fd;
	rep->fds[rep->nfds].events = repoll_map_events(event->events);
	rep->data[rep->nfds++] = event->data;
	return 0;
}

static int repoll_del(struct repoll *rep, int fd)
{
	int i;

	for (i = 0; i < rep->nfds; i++) {
		if (rep->fds[i].fd == fd) {
			rep->fds[i].fd = rep->fds[rep->nfds - 1].fd;
			rep->data[i] = rep->data[--rep->nfds];
			return 0;
		}
	}
	return ERR(EBADF);
}

static int repoll_mod(struct repoll *rep, int fd, struct epoll_event *event)
{
	int i;

	for (i = 0; i < rep->nfds; i++) {
		if (rep->fds[i].fd == fd) {
			rep->fds[i].events = repoll_map_events(events->events);
			rep->data[i] = event->data;
			return 0;
		}
	}
	return ERR(EBADF);
}

int repoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	struct repoll *rep;
	int ret;

	rep = idm_lookup(&ep_idm, epfd);
	if (!rep)
		return ERR(EBADF);

	pthread_mutex_lock(&ep_mut);
	switch (op) {
	case EPOLL_CTL_ADD:
		ret = repoll_add(rep, fd, event);
		break;
	case EPOLL_CTL_DEL:
		ret = repoll_del(rep, fd);
		break;
	case EPOLL_CTL_MOD:
		ret = repoll_mod(rep, fd, event);
		break;
	default:
		ret = ERR(EINVAL);
		break;
	}
	pthread_mutex_unlock(&ep_mut);
	return ret;
}

int repoll_close(int epfd)
{
	struct repoll *rep;

	rep = idm_lookup(&ep_idm, epfd);
	if (!rep)
		return ERR(EBADF);

	rep_remove(rep);
	close(rep->epfd);
	free(rep->fds);
	free(rep);
	return 0;
}

int repoll_create1(int flags)
{
	struct repoll *rep;

	rs_configure();

	rep = calloc(1, sizeof(*rep));
	if (!rep)
		return ERR(ENOMEM);

	rep->epfd = epoll_create1(flags);
	if (rep->epfd < 0)
		goto err1;

	ret = rep_insert(rep);
	if (ret < 0)
		goto err2;

	return 0;

err2:
	close(rep->epfd);
err1:
	free(rep);
	return ret;
}

int repoll_create(int size)
{
	return repoll_create1(0);
}

