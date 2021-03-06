//Author: lhanjian
//Start 20130407

#include "event_lea.h"
#include "lea_heap.h"
#include <errno.h>

static inline res_t
lt_add_to_epfd(int epfd, event_t *event, int mon_fd, flag_t flag)
{
    res_t res;

    struct epoll_event ev;

    ev.events = EPOLLRDHUP;
    if (flag & LV_FDRD)
        ev.events |= EPOLLIN;
    if (flag & LV_FDWR)
        ev.events |= EPOLLOUT;
    if (flag & LV_CONN)  
        ev.events |= EPOLLET;
    
    ev.data.ptr = event;
    
    res = epoll_ctl(epfd, EPOLL_CTL_ADD, mon_fd, &ev);
	if (res) {
		perror("epoll_ctl");
	}

    return res;
}

void
lt_ev_init_(event_t *event,//, deleted_evlist_t *deletedlist,//event_t *event,
        flag_t flag_set, int fd, //int epfd,
        func_t callback, void *arg, int deleted)
{
    event->callback = callback;
    event->arg = arg;
    event->flag = flag_set;
    event->fd = fd;
    event->deleted = deleted;
    event->next_active_ev = NULL;

}
/*
static event_t *
lt_add_to_readylist(ready_evlist_t *readylist, //deleted_evlist_t* deletedlist,
  flag_t flag_set, int fd, func_t callback, void *arg)
{
    event_t *event = lt_ev_constructor_(readylist, //deletedlist, 
            flag_set, fd, callback, arg, UNDELETED);
    if (event == NULL)
        return NULL;
    readylist->event_len++;

    return event;
}
*/
event_t *
lt_new_event(base_t *base)
{
	event_t *ev = NULL;
	event_t *free_ev = base->free_ev_head->next;
	if ((ev = free_ev)) {
		free_ev->prev->next = free_ev->next;
		free_ev->next->prev = free_ev->prev;
	} else {
		ev = lt_alloc(&base->event_pool_manager);
	}

	return ev;
}

void
lt_del_event(base_t *base, event_t *event)
{
	event_t *free_ev = base->free_ev_head->next;
	if (free_ev) {
		event->prev = free_ev->prev;
		event->next = free_ev;
		free_ev->prev = event;
		free_ev->prev->next = event;
	} else {
		base->free_ev_head->next = event;
		event->prev = base->free_ev_head;
	}
	return;
}

int
lt_new_post_callback(base_t *base, func_t callback, int fd, void *arg)//.*, flag_t flag)
{
	event_t *event = lt_new_event(base);
    lt_ev_init_(event, LV_ONESHOT, fd,
            callback, arg, UNDELETED);
/*    event_t *active_head_ev = base->activelist.head;
    if (active_head_ev) {
        active_head_ev->next_active_ev = ev;
        base->activelist.tail = ev;
    } else {
        base->activelist.head = ev;
        base->activelist.tail = ev;
    }*/

    return 0;
}

int 
epoll_init()
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create1");
    }
    return epfd;
}

base_t *
lt_base_init(void)
{
    base_t *base = malloc(sizeof(base_t));

    if (!base) {
        return NULL;
    }

    int epfd = epoll_init();
    if (epfd == -1) {
        free(base);
        return NULL;
    }

    base->epfd = epfd;
    base->free_ev_head = malloc(sizeof(event_t));
    base->free_ev_head->next = NULL;
    base->activelist.head = NULL;
    /*base->readylist.event_pool_manager = */

    lt_new_memory_pool_manager(&base->event_pool_manager,
    		sizeof(event_t), EVENT_POOL_LENGTH);

    min_heap_constructor_(&base->timeheap);
/*
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) {
        perror("timerfd_create");
    }
    base->timerfd = tfd;
    */

    return base;
}

event_t *
lt_io_add(base_t *base, int   fd, flag_t flag_set,
       func_t callback, void *arg, to_t timeout)
{
    event_t *event = lt_new_event(base);
    lt_ev_init_(event, flag_set, fd, callback, arg, UNDELETED);
	if (timeout >= 0) {
		event->endtime = lt_timeout_add(base, event, timeout);//lt_timeout_add TODO
    } else {
        event->min_heap_idx = -1;
    }
    
    lt_add_to_epfd(base->epfd, event, fd, flag_set);
    event->base = base;

    return event;
}

static inline res_t
lt_mod_to_epfd(int epfd, event_t *event, int mon_fd, flag_t flag) 
{
    res_t res;

    struct epoll_event ev;

    ev.events = EPOLLRDHUP;
    if (flag & LV_FDRD)
        ev.events |= EPOLLIN;
    if (flag & LV_FDWR)
        ev.events |= EPOLLOUT;
    if (flag & LV_CONN)
        ev.events |= EPOLLET;

    ev.data.ptr = event;
    
    res = epoll_ctl(epfd, EPOLL_CTL_MOD, mon_fd, &ev);
    if (res) {
        perror("epoll_ctl");
    }

    return res;
}

event_t *
lt_io_mod(base_t *base, event_t *ev, flag_t new_flag_set,//自行获取原有的flag
        func_t callback, void *arg, to_t timeout)
{
    if (ev) { 
        if (timeout >= 0) {
            ev->endtime = lt_timeout_add(base, ev, timeout);
        } else {
            ev->min_heap_idx = -1;
        } 

        ev->flag = new_flag_set;
        ev->callback = callback;
        ev->arg = arg;

        lt_mod_to_epfd(base->epfd, ev, ev->fd, new_flag_set);
    }
    return ev;
}

static void//res_t
lt_ev_process_and_moveout(base_t *base, /*active_evlist_t *actlist,*/ lt_time_t nowtime)
{
    active_evlist_t *actlist = &base->activelist;
    //ready_evlist_t *readylist = &base->readylist;
    for (event_t *event = actlist->head; 
            event; 
            event = event->next_active_ev) {//Why not use Tree?
		if (lt_ev_check_timeout(event, nowtime)) {
            event->deleted = 1;
        } else if (event->deleted){ //cluster some event and del it;
//            lt_io_remove(<#base_t *base#>, <#event_t *ev#>)
        } else {
            event->callback(event, event->arg);
            event->deleted = event->flag & LV_ONESHOT;//TODO
        }
    }
    actlist->head = NULL;
    actlist->tail = NULL;
    return;
}

static inline void
lt_loop_init_actlist(base_t *base, struct epoll_event ev_array[], int ready)
{
    int i = 0;
    active_evlist_t *actlist = &base->activelist;

    for (; i < ready; i++) {
        event_t *ev = (event_t *)ev_array[i].data.ptr;

        if (ev_array[i].events & (EPOLLIN|EPOLLOUT)) {
            if (ev->flag & LV_LAG ) {
                actlist->head = ev;//lag
                actlist->tail = ev;
                break;
            } else {
                ev->callback(ev, ev->arg);//directly callback
            } 
        } else if(ev_array[i].events & EPOLLRDHUP) {
            //TODO
        }
    }

    i++;
    event_t *ev_prev = actlist->head;
    for (; i < ready; i++) {
        event_t *ev = (event_t *)ev_array[i].data.ptr;
        if ((ev->flag & LV_LAG) && (ev_array[i].events & (EPOLLIN|EPOLLOUT)) ) {
            ev_prev->next_active_ev = ev;
            ev_prev = ev;
        } else {
            ev->callback(ev, ev->arg); 
        }
    }
    actlist->tail = ev_prev;

    return ;
}

int timerfd_expiration(struct event *event, void *arg)
{
    uint64_t value;
    ssize_t rv = read(event->fd, &value, sizeof(uint64_t));
    if (rv != sizeof(uint64_t)) {
        perror("timerfd read\n");
    }
    
    return 0;
}
/*
void timerfd_epoll_init(struct timespec timeout, base_t *base)
{
    int tfd = base->timerfd;
    struct itimerspec old_value = {
    };
    struct itimerspec new_value = {
        .it_value = {
            .tv_sec = 0,
            .tv_nsec = 0
        },
        .it_interval = {
            .tv_sec = timeout.tv_sec,
            .tv_nsec = timeout.tv_nsec
        }
    };

    struct epoll_event ev = {
        .data = { .fd = tfd},
        .events = EPOLLIN
    };


    timerfd_settime(tfd, 0, &new_value, &old_value);
    
    lt_io_add(base, tfd, LV_FDRD|LV_CONN, timerfd_expiration, NULL, NO_TIMEOUT);
    return ;
}
*/
res_t 
lt_base_loop(base_t *base, int timeout)
{
	lt_time_t start, /*now,*/ after;

    long long diff;
    int ready = 0;

    start = lt_gettime();
//    timerfd_epoll_init(timeout, base);
    int epevents_len = INIT_EPEV;
    int ep_to = timeout;

    for (;;) {
        struct epoll_event epevents[epevents_len];
        ready = epoll_wait(base->epfd, /*base->*/epevents, 
				/*base->readylist.event_len*/epevents_len, ep_to);//TODO:timerfd_create
        if (ready == -1) {
            int errsv = errno;
            if (errsv == EINTR) {
                //
            } else {
                perror("epoll_wait");
                return -1;
            }
        }
        
        after = lt_gettime();
		diff = lt_time_a_sub_b(after, start);//SUB TODO
		if (time_a_gt_b(diff,>,timeout)) {
			fprintf(stderr, "loop expired\n");
			break;
		}

		lt_loop_init_actlist(base, epevents, ready);

        lt_ev_process_and_moveout(base, after);
/*
        if (ready == epevents_len)
            epevents_len *= 2;*/
    }

    return 0;
}

lt_time_t
lt_gettime()
{
    int rv;
    lt_time_t time_now;

    rv = clock_gettime(CLOCK_MONOTONIC, &time_now);

    if (rv == -1) {
        perror("gettime error");
    }

	return time_now;
}

res_t
lt_remove_from_epfd(int epfd, event_t *event, int mon_fd, flag_t flag)
{
    res_t res;

    res = epoll_ctl(epfd, EPOLL_CTL_DEL, mon_fd, NULL);
    if (res) {
        perror("epoll_ctl DEL");
    }

    return res;
}
/*
void//res_t
lt_remove_from_readylist(event_t *ev, ready_evlist_t *readylist) 
{
    lt_free(readylist->event_pool, ev);//no order
    readylist->event_len--;
}
*/
void//res_t
lt_io_remove(base_t *base, event_t *ev)//Position TODO
{
    if (ev->min_heap_idx != -1) {
        min_heap_erase_(&base->timeheap, ev);//First erase heap
    } 

 //   lt_remove_from_readylist(ev, &base->readylist);
    lt_remove_from_epfd(base->epfd, ev, ev->fd, 0);//For active event, it's different with Libevent.

    ev->deleted = 1;//For active event, it's different with Libevent. 
}

res_t
lt_ev_check_timeout(event_t *ev, lt_time_t nowtime)
{ 
    time_t sec_diff = ev->endtime.tv_sec - nowtime.tv_sec;
    long nsec_diff = ev->endtime.tv_nsec - nowtime.tv_nsec;

    if (sec_diff > 0) {
        return 1;
    } else if (sec_diff == 0) {
        if (nsec_diff >= 0) { return 1;}
        else {return 0;}
    } else /*(sec_diff < 0)*/ {
        return 0;
    }
}

lt_time_t
lt_timeout_add(base_t *base, event_t *ev, to_t to)//add to a tree?
{
    lt_time_t endtime = lt_time_addition(lt_gettime(), to);
    
    min_heap_elem_init_(ev);
    min_heap_push_(&base->timeheap, ev);

    return endtime;
}

lt_time_t
lt_time_addition(lt_time_t time, to_t to)
{
    long nsec, sec;
    nsec = to + time.tv_nsec;
    sec  = time.tv_sec;

    while (nsec > 1000000000L) {//1E9 LONG
        nsec = nsec - 1000000000L;
        sec += 1L;
    }

    return (lt_time_t) { 
        .tv_sec  = sec, 
        .tv_nsec = nsec 
    };
}

long long
lt_time_a_sub_b(lt_time_t a, lt_time_t b)
{ return (a.tv_sec*1000000000LL + a.tv_nsec - b.tv_sec * 1000000000LL - b.tv_nsec); }


int 
lt_ignore_sigpipe()
{
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction SIGPIPE:");
        return -1;
    }
    return 0;
}
