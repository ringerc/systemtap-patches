/* -*- linux-c -*- 
 * Perf Header File
 * Copyright (C) 2006 Red Hat Inc.
 * Copyright (C) 2010 Red Hat Inc.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

#ifndef _PERF_H_
#define _PERF_H_

/** @file perf.h
 * @brief Header file for performance monitoring hardware support
 */

struct _Perf {
	/* per-cpu data. allocated with _stp_alloc_percpu() */
	struct perf_event **pd;
        perf_overflow_handler_t callback;
};

typedef struct _Perf *Perf;

static Perf _stp_perf_init (struct perf_event_attr *attr,
			    perf_overflow_handler_t callback);

static void _stp_perf_del (Perf pe);

#endif /* _PERF_H_ */
