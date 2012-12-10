/*
Thread bug detector. Background:

Existing thread bug detectors can only identify a non-exclusive access (r/w ) in the moment it occurs at runtime. However
a few data races only occur under certain rare conditions. This wrapper can force some of these conditions to reveal
and is perfect to use in combination with existing thread bug detectors but can also be used alone.
*/

#ifndef TIGHTDB_PTHREAD_TEST_HPP
#define TIGHTDB_PTHREAD_TEST_HPP

unsigned int ptf_fastrand() 
{
    // Must be fast because important edge case is 0 delay. Not thread safe, but that just adds randomnes.
    static unsigned int u = 1;
    static unsigned int v = 1;
    v = 36969*(v & 65535) + (v >> 16);
    u = 18000*(u & 65535) + (u >> 16);
    return (v << 16) + u;
}

void ptf_randsleep(void)
{
    unsigned int r = ptf_fastrand() % 1000;
    unsigned long long ms = 500000; // on 2 ghz

    if (r < 200) {
        return;
    }
    else if (r < 300) {
        // Wait 0 - 1 ms, probably wake up in current time slice
        size_t w = ms / 10 * (ptf_fastrand() % 10);
        for (volatile size_t t = 0; t < w; ++t) {
        }
    }
    else if (r < 306) {
        // Wait 0 - 100 ms, maybe wake up in different time slice
        size_t w = ms * (ptf_fastrand() % 100);
        for (volatile size_t t = 0; t < w; ++t) {
        }
    }
    else if (r < 800) {
        // Wake up in time slice earlier than sleep(0) on some OSes
        sched_yield();
    }
    else if (r < 999) {
        // Wake up in time slice according to normal OS scheduling
        usleep(0);
    }
    else {
        size_t w = ptf_fastrand() % 100;
        usleep(w);
    }
}

int ptf_pthread_mutex_trylock(pthread_mutex_t * mutex)
{
	ptf_randsleep();
	int i = pthread_mutex_trylock(mutex);
	ptf_randsleep();
	return i;
}

int ptf_pthread_barrier_wait(pthread_barrier_t *barrier)
{
	ptf_randsleep();
	int i = pthread_barrier_wait(barrier);
	ptf_randsleep();
	return i;
}

#define ptf_surround(arg) \
	ptf_randsleep(); \
	arg; \
	ptf_randsleep();

#define pthread_mutex_lock(mutex) ptf_surround(pthread_mutex_lock(mutex))
#define pthread_mutex_unlock(mutex) ptf_surround(pthread_mutex_unlock(mutex))
#define pthread_cond_wait(mutex, cond) ptf_surround(pthread_cond_wait(mutex, cond))
#define pthread_cond_broadcast(cond) ptf_surround(pthread_cond_broadcast(cond))
#define pthread_cond_signal(cond) ptf_surround(pthread_cond_signal(cond))
#define pthread_mutex_trylock ptf_pthread_mutex_trylock
#define pthread_barrier_wait ptf_pthread_barrier_wait

#endif