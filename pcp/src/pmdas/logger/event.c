/*
 * event support for the Logger PMDA
 *
 * Copyright (c) 2011 Red Hat Inc.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <pcp/pmapi.h>
#include <pcp/impl.h>
#include <pcp/pmda.h>
#include "event.h"
#include "percontext.h"
#include "util.h"

#define BUF_SIZE 1024

struct event {
    TAILQ_ENTRY(event) events;
    int clients;
    char buffer[BUF_SIZE];
};

TAILQ_HEAD(tailhead, event);

struct EventFileData {
    struct LogfileData *logfile;
    int			fd;
    pid_t	        pid;
    int			numclients;
    struct tailhead	head;
};
static struct EventFileData *file_data_tab;

static int eventarray;
static int numlogfiles;

struct ctx_client_data {
    unsigned int	active_logfile;
    struct event      **last;
};

static void event_cleanup(void);

static void *
ctx_start_callback(int ctx)
{
    struct ctx_client_data *c = ctx_get_user_data();

    if (c == NULL) {
	c = calloc(numlogfiles, sizeof(struct ctx_client_data));
	if (c == NULL) {
	    __pmNotifyErr(LOG_ERR, "allocation failure");
	    return NULL;
	}
    }
    return c;
}

static void
ctx_end_callback(int ctx, void *user_data)
{
    struct ctx_client_data *c = user_data;
    
    event_cleanup();
    if (c != NULL)
	free(c);
    return;
}

void
event_init(pmdaInterface *dispatch, struct LogfileData *logfiles,
	   int nlogfiles)
{
    int i;

    numlogfiles = nlogfiles;
    if (numlogfiles <= 0 || logfiles == NULL) {
	__pmNotifyErr(LOG_ERR, "no logfiles");
	exit(1);
    }

    eventarray = pmdaEventNewArray();

    ctx_register_callbacks(ctx_start_callback, ctx_end_callback);

    /* Allocate our EventFileData table. */
    file_data_tab = calloc(numlogfiles, sizeof(struct EventFileData));
    if (file_data_tab == NULL) {
	fprintf(stderr, "%s: allocation error: %s\n", __FUNCTION__,
		strerror(errno));
	return;
    }

    /* Fill it the table. */
    for (i = 0; i < numlogfiles; i++) {
	file_data_tab[i].logfile = &logfiles[i];
	TAILQ_INIT(&file_data_tab[i].head); /* initialize queue */
    }

    /* We can't really use select() on the logfiles.  Why?  If the
     * logfile is a normal file, select will (continually) return EOF
     * after we've read all the data.  Then we tried a custom main
     * that that read data before handling any message we get on the
     * control channel.  That didn't work either, since the client
     * context wasn't set up yet (since that is the 1st control
     * message).  So, now we read data inside the event fetch
     * routine. */

    /* Try to open all the logfiles to monitor */
    for (i = 0; i < numlogfiles; i++) {
	size_t pathlen = strlen(logfiles[i].pathname);

	/* We support 2 kinds of PATHNAMEs:
	 *
	 * (1) Regular paths.  These paths are opened normally.
	 *
	 * (2) Pipes.  If the path ends in '|', the filename is
	 * interpreted as a command which pipes input to us.
	 *
	 * Handle both.
	 */

	if (logfiles[i].pathname[pathlen - 1] != '|') {
	    file_data_tab[i].fd = open(logfiles[i].pathname,
				       O_RDONLY|O_NONBLOCK);
	}
	else {
	    char cmd[MAXPATHLEN];

	    strncpy(cmd, logfiles[i].pathname, sizeof(cmd));
	    cmd[pathlen - 1] = '\0';	/* get rid of the '|' */
	    /* Remove all trailing whitespace. */
	    rstrip(cmd);

	    /* Start the command. */
	    file_data_tab[i].fd = start_cmd(cmd, &file_data_tab[i].pid);
	}

	if (file_data_tab[i].fd < 0) {
	    __pmNotifyErr(LOG_ERR, "open failure on %s", logfiles[i].pathname);
	    /* Cleanup. */
	    event_shutdown();
	    exit(1);
	}

	/* Skip to the end. */
	lseek(file_data_tab[i].fd, 0, SEEK_END);
    }
}

static int
event_create(int logfile)
{
    ssize_t c;

    /* Allocate a new event. */
    struct event *e = malloc(sizeof(struct event));
    if (e == NULL) {
	__pmNotifyErr(LOG_ERR, "allocation failure");
	return -1;
    }

    /* Read up to BUF_SIZE bytes at a time. */
    c = read(file_data_tab[logfile].fd, e->buffer, sizeof(e->buffer) - 1);

    /* If we've got EOF (0 bytes read) or EBADF (fd isn't valid - most
     * likely a closed pipe), just ignore the error. */
    if (c == 0 || (c < 0 && errno == EBADF)) {
	free(e);
	return 0;
    }
    if (c < 0) {
	__pmNotifyErr(LOG_ERR, "read failure on %s: %s",
		      file_data_tab[logfile].logfile->pathname,
		      strerror(errno));
	free(e);
	return -1;
    }

    /* Update logfile event stats. */
    file_data_tab[logfile].logfile->count++;
    file_data_tab[logfile].logfile->bytes += c;

    /* Store event in queue. */
    e->clients = file_data_tab[logfile].numclients;
    e->buffer[c] = '\0';
    TAILQ_INSERT_TAIL(&file_data_tab[logfile].head, e, events);
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_APPL1)
	__pmNotifyErr(LOG_INFO, "Inserted item, clients = %d.", e->clients);
#endif
    return 0;
}

int
event_get_clients_per_logfile(unsigned int logfile)
{
    return file_data_tab[logfile].numclients;
}

int
event_fetch(pmValueBlock **vbpp, unsigned int logfile)
{
    struct event *e, *next;
    struct timeval stamp;
    pmAtomValue atom;
    int rc;
    int records = 0;
    struct ctx_client_data *c = ctx_get_user_data();
    
    /* Make sure the we keep track of which clients are interested in
     * which logfiles is up to date. */
    if (c[logfile].active_logfile == 0) {
	c[logfile].active_logfile = 1;
	c[logfile].last = file_data_tab[logfile].head.tqh_last;
	file_data_tab[logfile].numclients++;
    }

    /* Update the event queue with new data (if any). */
    if ((rc = event_create(logfile)) < 0)
	return rc;

    if (vbpp == NULL)
	return -1;
    *vbpp = NULL;

    pmdaEventResetArray(eventarray);
    gettimeofday(&stamp, NULL);
    if ((rc = pmdaEventAddRecord(eventarray, &stamp, PM_EVENT_FLAG_POINT)) < 0)
	return rc;

    e = *c[logfile].last;
    while (e != NULL) {
	/* Add the string parameter.  Note that pmdaEventAddParam()
	 * copies the string, so we can free it soon after. */
	atom.cp = e->buffer;
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_APPL1)
	    __pmNotifyErr(LOG_INFO, "Adding param: %s", e->buffer);
#endif
	rc = pmdaEventAddParam(eventarray,
			       file_data_tab[logfile].logfile->pmid,
			       PM_TYPE_STRING, &atom);
	if (rc < 0)
	    return rc;
	records++;

	/* Get the next event. */
	next = e->events.tqe_next;

	/* Remove the current one (if its use count is at 0). */
	if (--e->clients <= 0) {
	    TAILQ_REMOVE(&file_data_tab[logfile].head, e, events);
	    free(e);
	}

	/* Go on to the next event. */
	e = next;
    }

    /* Update queue pointer. */
    c[logfile].last = file_data_tab[logfile].head.tqh_last;

    if (records > 0)
	*vbpp = (pmValueBlock *)pmdaEventGetAddr(eventarray);
    else
	*vbpp = NULL;
    return 0;
}

static void
event_cleanup(void)
{
    struct event *e, *next;
    struct ctx_client_data *c = ctx_get_user_data();
    int logfile;

    /* We've lost a client.  Cleanup. */
    for (logfile = 0; logfile < numlogfiles; logfile++) {
	if (c[logfile].active_logfile == 0)
	    continue;

	file_data_tab[logfile].numclients--;
	e = *c[logfile].last;
	while (e != NULL) {
	    /* Get the next event. */
	    next = e->events.tqe_next;

	    /* Remove the current one (if its use count is at 0). */
	    if (--e->clients <= 0) {
		TAILQ_REMOVE(&file_data_tab[logfile].head, e, events);
		free(e);
	    }

	    /* Go on to the next event. */
	    e = next;
	}
    }
}

void
event_shutdown(void)
{
    int i;

    __pmNotifyErr(LOG_INFO, "%s: Shutting down...", __FUNCTION__);
    for (i = 0; i < numlogfiles; i++) {
	if (file_data_tab[i].pid != 0) {
	    (void) stop_cmd(file_data_tab[i].pid);
	    file_data_tab[i].pid = 0;
	}
	if (file_data_tab[i].fd > 0) {
	    close(file_data_tab[i].fd);
	    file_data_tab[i].fd = 0;
	}
    }
}
