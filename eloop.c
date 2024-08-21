#include <inttypes.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <unistd.h>
#include <stdio.h>

#include "eloop.h"

typedef struct _TimeoutStruct {
    int64_t expire;
    EloopCallback cb;
    int tag;
    void *data;
    int time_slice;
    struct _TimeoutStruct *next;
    struct _TimeoutStruct *prev;
} TimeoutStruct;

typedef struct _InOutStruct {
    int fd;
    EloopCondition cond;
    EloopInputCallback cb;
    int tag;
    void *data;
    struct _InOutStruct *next;
    struct _InOutStruct *prev;
} InOutStruct;

static TimeoutStruct *timeout_head = 0;
static InOutStruct *inout_head = 0;
static int timeout_tag = 1;
static int inout_tag = 1;
static int inout_num = 0;

static void
insert_timeout(TimeoutStruct *me)
{
    TimeoutStruct *ts = timeout_head, *prev = 0;
    while(ts) {
	if (ts->expire > me->expire) {
	    /* prev me ts */
	    me->next = ts;
	    me->prev = prev;
	    if (prev) {
		prev->next = me;
	    } else {
		timeout_head = me;
	    }
	    ts->prev = me;
	    return;
	}
	prev = ts;
	ts = ts->next;
    }
    me->next = 0;
    me->prev = prev;
    if (prev) {
	prev->next = me;
    } else {
	timeout_head = me;
    }
}

static void
remove_timeout(TimeoutStruct *me)
{
    if (me->prev) {
	me->prev->next = me->next;
    } else {
	timeout_head = me->next;
    }
    if (me->next) {
	me->next->prev = me->prev;
    }
}

static void
invoke_timeout(TimeoutStruct *me)
{
    int ret = (*me->cb)(me->data);
    remove_timeout(me);
    if (ret >= 0) {
	me->expire += me->time_slice * 1000;
	insert_timeout(me);
    }
}

static void
insert_input(InOutStruct *me)
{
    if (inout_head) inout_head->prev = me;
    me->next = inout_head;
    me->prev = 0;
    inout_head = me;
    inout_num++;
}

static void
remove_input(InOutStruct *me)
{
    if (me->prev) {
	me->prev->next = me->next;
    } else {
	inout_head = me->next;
    }
    if (me->next) {
	me->next->prev = me->prev;
    }
    inout_num--;
}

static void
invoke_inout(InOutStruct *me, EloopCondition cond)
{
    int ret = (*me->cb)(me->fd, cond, me->data);
    if (ret < 0) {
	remove_input(me);
    }
}

static int64_t
time_get_current(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

int
eloop_add_timeout(unsigned int time_slice, EloopCallback cb, void *data)
{
    TimeoutStruct *ts = g_new(TimeoutStruct, 1);
    ts->cb = cb;
    ts->data = data;
    ts->expire = time_get_current() + time_slice * 1000;
    ts->time_slice = time_slice;
    ts->tag = timeout_tag++;
    insert_timeout(ts);
    return ts->tag;
}

void 
eloop_remove_timeout(int tag)
{
    TimeoutStruct *ts = timeout_head;
    while(ts) {
	if (ts->tag == tag) {
	    remove_timeout(ts);
	    free(ts);
	    return;
	}
	ts = ts->next;
    }
}

int
eloop_add_input(int fd, EloopCondition cond, EloopInputCallback cb, void *data)
{
    InOutStruct *io = g_new(InOutStruct, 1);
    io->fd = fd;
    io->cond = cond;
    io->cb = cb;
    io->data = data;
    io->tag = inout_tag++;
    insert_input(io);
    return io->tag;
}

void 
eloop_remove_input(int tag)
{
    InOutStruct *io = inout_head;
    while(io) {
	if (io->tag == tag) {
	    remove_input(io);
	    free(io);
	    return;
	}
	io = io->next;
    }
}

static int loop_cont;

static void
main_iter(void)
{
    int timeout = -1;
    struct pollfd *ufds = 0;
    int nfds = inout_num;
    int i;
    int n;

    if (timeout_head) {
	timeout = timeout_head->expire - time_get_current();
	timeout /= 1000; /* us -> ms */
	while(timeout < 0) {
	    invoke_timeout(timeout_head);
	    if (!timeout_head) break;
	    timeout = timeout_head->expire - time_get_current();
	    timeout /= 1000; /* us -> ms */
	}
	if (!timeout_head) timeout = -1;
    }
    if (!loop_cont) return;
    if (nfds > 0) {
	InOutStruct *io = inout_head;
	ufds = g_new(struct pollfd, nfds);
	for(i=0;i<nfds;i++) {
	    ufds[i].fd = io->fd;
	    ufds[i].events = 0;
	    if (io->cond & ELOOP_READ) {
		ufds[i].events |= POLLIN;
	    }
	    if (io->cond & ELOOP_WRITE) {
		ufds[i].events |= POLLOUT;
	    }
	    io = io->next;
	}
    }
    n = poll(ufds, nfds, timeout);
    if (n == 0) {
	invoke_timeout(timeout_head);
    } else if (n > 0) {
	InOutStruct *ts = inout_head;
	for(i=0;i<nfds;i++) {
	    InOutStruct *next = ts->next;
	    if (ufds[i].revents) {
		int cond = 0;
		if (ufds[i].revents & POLLIN) cond |= ELOOP_READ;
		if (ufds[i].revents & POLLOUT) cond |= ELOOP_WRITE;
		if (ufds[i].revents & POLLERR) cond |= ELOOP_EXCEPTION;
		invoke_inout(ts, cond);
	    }
	    ts = next;
	}
    } else {
	perror("poll");
    }
    free(ufds);
}

void 
eloop_main(void)
{
    loop_cont = 1;
    do {
	main_iter();
    } while(loop_cont);
}

void
eloop_quit(void)
{
    loop_cont = 0;
}

//#define TEST

#ifdef TEST
int
timeout_cb(void *data)
{
    printf("time out\n");
    //eloop_quit();
    return -1;
}

int
timeout1_cb(void *data)
{
    printf("time out1\n");
    return 1;
}

int
timeout2_cb(void *data)
{
    printf("time out2\n");
    return 1;
}

int
input_cb(int fd, EloopCondition cond, void *data)
{
    char buf[256];
    int n;
    n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
	printf("inout read error\n");
	return -1;
    }
    printf("input: %s\n", buf);
    return 1;
}

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <X11/Xlib.h>
static Display *dpy;

static int
create_xwin(void)
{
    XSetWindowAttributes attr;
    int mask;
    Window win;
    dpy = XOpenDisplay(NULL);
    attr.background_pixel = 0;
    win = XCreateWindow(dpy, DefaultRootWindow(dpy),
      0, 0, 400, 300, 0, CopyFromParent,
      InputOutput, CopyFromParent, CWBackPixel, &attr);
    mask = KeyPressMask | KeyReleaseMask;
    XSelectInput(dpy, win, mask);
    XMapWindow(dpy, win);
    printf("map window\n");
    XFlush(dpy);
    return ConnectionNumber(dpy);
}

static int
xinput_cb(int fd, EloopCondition con, void *data)
{
    char buf[16];
    int keysym;
    int compose;
    printf("xinput cb\n");
    do {
	XEvent ev;
	XNextEvent(dpy, &ev);
	switch(ev.type) {
	    case KeyPress: 
		XLookupString(&ev.xkey, buf, 16, &keysym, &compose);
		printf("key press: %s\n", buf);
		break;
	    case KeyRelease: 
		XLookupString(&ev.xkey, buf, 16, &keysym, &compose);
		printf("key release: %s\n", buf);
		break;
	}
    } while(XPending(dpy));
    return 1;
}

int
main(int argc, char *argv[])
{
    int64_t cur = time_get_current();
    int fifo = open("ftest", O_RDONLY|O_NONBLOCK);
    int fd;
    eloop_add_timeout(1000, timeout1_cb, 0);
    eloop_add_timeout(1500, timeout2_cb, 0);
    eloop_add_timeout(3000, timeout_cb, 0);
    eloop_add_input(fifo, ELOOP_READ, input_cb, 0);
    fd = create_xwin();
    eloop_add_input(fd, ELOOP_READ, xinput_cb, 0);
    eloop_main();
    printf("time delta = %d\n", (int)(time_get_current() - cur) / 1000);
    return 0;
}
#endif
