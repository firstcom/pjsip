/* $Header: /pjproject-0.3/pjlib/src/pjlib-test/ioq_perf.c 4     10/29/05 11:51a Bennylp $ */
/*
 * $Log: /pjproject-0.3/pjlib/src/pjlib-test/ioq_perf.c $
 * 
 * 4     10/29/05 11:51a Bennylp
 * Version 0.3-pre2.
 * 
 * 3     14/10/05 11:31 Bennylp
 * More generalized test method, works for UDP too.
 * 
 * 2     10/14/05 12:26a Bennylp
 * Finished error code framework, some fixes in ioqueue, etc. Pretty
 * major.
 * 
 * 1     10/11/05 3:52p Bennylp
 * Created.
 *
 */
#include "test.h"
#include <pjlib.h>
#include <pj/compat/high_precision.h>

/**
 * \page page_pjlib_ioqueue_perf_test Test: I/O Queue Performance
 *
 * Test the performance of the I/O queue, using typical producer
 * consumer test. The test should examine the effect of using multiple
 * threads on the performance.
 *
 * This file is <b>pjlib-test/ioq_perf.c</b>
 *
 * \include pjlib-test/ioq_perf.c
 */

#if INCLUDE_IOQUEUE_PERF_TEST

#ifdef _MSC_VER
#   pragma warning ( disable: 4204)     // non-constant aggregate initializer
#endif

#define THIS_FILE	"ioq_perf"
//#define TRACE_(expr)	PJ_LOG(3,expr)
#define TRACE_(expr)


static pj_bool_t thread_quit_flag;
static pj_status_t last_error;
static unsigned last_error_counter;

/* Descriptor for each producer/consumer pair. */
typedef struct test_item
{
    pj_sock_t       server_fd, 
                    client_fd;
    pj_ioqueue_t    *ioqueue;
    pj_ioqueue_key_t *server_key,
                   *client_key;
    pj_size_t       buffer_size;
    char           *outgoing_buffer;
    char           *incoming_buffer;
    pj_size_t       bytes_sent, 
                    bytes_recv;
} test_item;

/* Callback when data has been read.
 * Increment item->bytes_recv and ready to read the next data.
 */
static void on_read_complete(pj_ioqueue_key_t *key, pj_ssize_t bytes_read)
{
    test_item *item = pj_ioqueue_get_user_data(key);
    pj_status_t rc;

    //TRACE_((THIS_FILE, "     read complete, bytes_read=%d", bytes_read));

    if (thread_quit_flag)
        return;

    if (bytes_read < 0) {
        pj_status_t rc = -bytes_read;
        char errmsg[128];

	if (rc != last_error) {
	    last_error = rc;
	    pj_strerror(rc, errmsg, sizeof(errmsg));
	    PJ_LOG(3,(THIS_FILE, "...error: read error, bytes_read=%d (%s)", 
		      bytes_read, errmsg));
	    PJ_LOG(3,(THIS_FILE, 
		      ".....additional info: total read=%u, total written=%u",
		      item->bytes_recv, item->bytes_sent));
	} else {
	    last_error_counter++;
	}
        bytes_read = 0;

    } else if (bytes_read == 0) {
        PJ_LOG(3,(THIS_FILE, "...socket has closed!"));
    }

    item->bytes_recv += bytes_read;
    
    /* To assure that the test quits, even if main thread
     * doesn't have time to run.
     */
    if (item->bytes_recv > item->buffer_size * 10000) 
	thread_quit_flag = 1;

    rc = pj_ioqueue_recv( item->ioqueue, item->server_key,
                          item->incoming_buffer, item->buffer_size, 0 );

    if (rc != PJ_SUCCESS && rc != PJ_EPENDING) {
	if (rc != last_error) {
	    last_error = rc;
	    app_perror("...error: read error", rc);
	} else {
	    last_error_counter++;
	}
    }
}

/* Callback when data has been written.
 * Increment item->bytes_sent and write the next data.
 */
static void on_write_complete(pj_ioqueue_key_t *key, pj_ssize_t bytes_sent)
{
    test_item *item = pj_ioqueue_get_user_data(key);
    
    //TRACE_((THIS_FILE, "     write complete: sent = %d", bytes_sent));

    if (thread_quit_flag)
        return;

    item->bytes_sent += bytes_sent;

    if (bytes_sent <= 0) {
        PJ_LOG(3,(THIS_FILE, "...error: sending stopped. bytes_sent=%d", 
                  bytes_sent));
    } 
    else {
        pj_status_t rc;

        rc = pj_ioqueue_write(item->ioqueue, item->client_key, 
                              item->outgoing_buffer, item->buffer_size);
        if (rc != PJ_SUCCESS && rc != PJ_EPENDING) {
            app_perror("...error: write error", rc);
        }
    }
}

/* The worker thread. */
static int worker_thread(void *arg)
{
    pj_ioqueue_t *ioqueue = arg;
    const pj_time_val timeout = {0, 100};
    int rc;

    while (!thread_quit_flag) {
        rc = pj_ioqueue_poll(ioqueue, &timeout);
	//TRACE_((THIS_FILE, "     thread: poll returned rc=%d", rc));
        if (rc < 0) {
            app_perror("...error in pj_ioqueue_poll()", pj_get_netos_error());
            return -1;
        }
    }
    return 0;
}

/* Calculate the bandwidth for the specific test configuration.
 * The test is simple:
 *  - create sockpair_cnt number of producer-consumer socket pair.
 *  - create thread_cnt number of worker threads.
 *  - each producer will send buffer_size bytes data as fast and
 *    as soon as it can.
 *  - each consumer will read buffer_size bytes of data as fast 
 *    as it could.
 *  - measure the total bytes received by all consumers during a
 *    period of time.
 */
static int perform_test(int sock_type, const char *type_name,
                        unsigned thread_cnt, unsigned sockpair_cnt,
                        pj_size_t buffer_size, 
                        pj_size_t *p_bandwidth)
{
    enum { MSEC_DURATION = 5000 };
    pj_pool_t *pool;
    test_item *items;
    pj_thread_t **thread;
    pj_ioqueue_t *ioqueue;
    pj_status_t rc;
    pj_ioqueue_callback ioqueue_callback;
    pj_uint32_t total_elapsed_usec, total_received;
    pj_highprec_t bandwidth;
    pj_timestamp start, stop;
    unsigned i;

    TRACE_((THIS_FILE, "    starting test.."));

    ioqueue_callback.on_read_complete = &on_read_complete;
    ioqueue_callback.on_write_complete = &on_write_complete;

    thread_quit_flag = 0;

    pool = pj_pool_create(mem, NULL, 4096, 4096, NULL);
    if (!pool)
        return -10;

    items = pj_pool_alloc(pool, sockpair_cnt*sizeof(test_item));
    thread = pj_pool_alloc(pool, thread_cnt*sizeof(pj_thread_t*));

    TRACE_((THIS_FILE, "     creating ioqueue.."));
    rc = pj_ioqueue_create(pool, sockpair_cnt*2, thread_cnt, &ioqueue);
    if (rc != PJ_SUCCESS) {
        app_perror("...error: unable to create ioqueue", rc);
        return -15;
    }

    /* Initialize each producer-consumer pair. */
    for (i=0; i<sockpair_cnt; ++i) {

        items[i].ioqueue = ioqueue;
        items[i].buffer_size = buffer_size;
        items[i].outgoing_buffer = pj_pool_alloc(pool, buffer_size);
        items[i].incoming_buffer = pj_pool_alloc(pool, buffer_size);
        items[i].bytes_recv = items[i].bytes_sent = 0;

        /* randomize outgoing buffer. */
        pj_create_random_string(items[i].outgoing_buffer, buffer_size);

        /* Create socket pair. */
	TRACE_((THIS_FILE, "      calling socketpair.."));
        rc = app_socketpair(PJ_AF_INET, sock_type, 0, 
                            &items[i].server_fd, &items[i].client_fd);
        if (rc != PJ_SUCCESS) {
            app_perror("...error: unable to create socket pair", rc);
            return -20;
        }

        /* Register server socket to ioqueue. */
	TRACE_((THIS_FILE, "      register(1).."));
        rc = pj_ioqueue_register_sock(pool, ioqueue, 
                                      items[i].server_fd,
                                      &items[i], &ioqueue_callback,
                                      &items[i].server_key);
        if (rc != PJ_SUCCESS) {
            app_perror("...error: registering server socket to ioqueue", rc);
            return -60;
        }

        /* Register client socket to ioqueue. */
	TRACE_((THIS_FILE, "      register(2).."));
        rc = pj_ioqueue_register_sock(pool, ioqueue, 
                                      items[i].client_fd,
                                      &items[i],  &ioqueue_callback,
                                      &items[i].client_key);
        if (rc != PJ_SUCCESS) {
            app_perror("...error: registering server socket to ioqueue", rc);
            return -70;
        }

        /* Start reading. */
	TRACE_((THIS_FILE, "      pj_ioqueue_recv.."));
        rc = pj_ioqueue_recv(ioqueue, items[i].server_key,
                             items[i].incoming_buffer, items[i].buffer_size,
			     0);
        if (rc != PJ_SUCCESS && rc != PJ_EPENDING) {
            app_perror("...error: pj_ioqueue_recv", rc);
            return -73;
        }

        /* Start writing. */
	TRACE_((THIS_FILE, "      pj_ioqueue_write.."));
        rc = pj_ioqueue_write(ioqueue, items[i].client_key,
                              items[i].outgoing_buffer, items[i].buffer_size);
        if (rc != PJ_SUCCESS && rc != PJ_EPENDING) {
            app_perror("...error: pj_ioqueue_write", rc);
            return -76;
        }

    }

    /* Create the threads. */
    for (i=0; i<thread_cnt; ++i) {
        rc = pj_thread_create( pool, NULL, 
                               &worker_thread, 
                               ioqueue, 
                               PJ_THREAD_DEFAULT_STACK_SIZE, 
                               PJ_THREAD_SUSPENDED, &thread[i] );
        if (rc != PJ_SUCCESS) {
            app_perror("...error: unable to create thread", rc);
            return -80;
        }
    }

    /* Mark start time. */
    rc = pj_get_timestamp(&start);
    if (rc != PJ_SUCCESS)
        return -90;

    /* Start the thread. */
    TRACE_((THIS_FILE, "     resuming all threads.."));
    for (i=0; i<thread_cnt; ++i) {
        rc = pj_thread_resume(thread[i]);
        if (rc != 0)
            return -100;
    }

    /* Wait for MSEC_DURATION seconds. 
     * This should be as simple as pj_thread_sleep(MSEC_DURATION) actually,
     * but unfortunately it doesn't work when system doesn't employ
     * timeslicing for threads.
     */
    TRACE_((THIS_FILE, "     wait for few seconds.."));
    do {
	pj_thread_sleep(1);

	/* Mark end time. */
	rc = pj_get_timestamp(&stop);

	if (thread_quit_flag) {
	    TRACE_((THIS_FILE, "      transfer limit reached.."));
	    break;
	}

	if (pj_elapsed_usec(&start,&stop)<MSEC_DURATION * 1000) {
	    TRACE_((THIS_FILE, "      time limit reached.."));
	    break;
	}

    } while (1);

    /* Terminate all threads. */
    TRACE_((THIS_FILE, "     terminating all threads.."));
    thread_quit_flag = 1;

    for (i=0; i<thread_cnt; ++i) {
	TRACE_((THIS_FILE, "      join thread %d..", i));
        pj_thread_join(thread[i]);
        pj_thread_destroy(thread[i]);
    }

    /* Close all sockets. */
    TRACE_((THIS_FILE, "     closing all sockets.."));
    for (i=0; i<sockpair_cnt; ++i) {
        pj_ioqueue_unregister(ioqueue, items[i].server_key);
        pj_ioqueue_unregister(ioqueue, items[i].client_key);
        pj_sock_close(items[i].server_fd);
        pj_sock_close(items[i].client_fd);
    }

    /* Destroy ioqueue. */
    TRACE_((THIS_FILE, "     destroying ioqueue.."));
    pj_ioqueue_destroy(ioqueue);

    /* Calculate actual time in usec. */
    total_elapsed_usec = pj_elapsed_usec(&start, &stop);

    /* Calculate total bytes received. */
    total_received = 0;
    for (i=0; i<sockpair_cnt; ++i) {
        total_received = items[i].bytes_recv;
    }

    /* bandwidth = total_received*1000/total_elapsed_usec */
    bandwidth = total_received;
    pj_highprec_mul(bandwidth, 1000);
    pj_highprec_div(bandwidth, total_elapsed_usec);
    
    *p_bandwidth = (pj_uint32_t)bandwidth;

    PJ_LOG(3,(THIS_FILE, "   %.4s    %d         %d        %3d us  %8d KB/s",
              type_name, thread_cnt, sockpair_cnt,
              -1 /*total_elapsed_usec/sockpair_cnt*/,
              *p_bandwidth));

    /* Done. */
    pj_pool_release(pool);

    TRACE_((THIS_FILE, "    done.."));
    return 0;
}

/*
 * main test entry.
 */
int ioqueue_perf_test(void)
{
    enum { BUF_SIZE = 512 };
    int i, rc;
    struct {
        int         type;
        const char *type_name;
        int         thread_cnt;
        int         sockpair_cnt;
    } test_param[] = 
    {
        { PJ_SOCK_DGRAM, "udp", 1, 1},
        { PJ_SOCK_DGRAM, "udp", 1, 2},
        { PJ_SOCK_DGRAM, "udp", 1, 4},
        { PJ_SOCK_DGRAM, "udp", 1, 8},
        { PJ_SOCK_DGRAM, "udp", 2, 1},
        { PJ_SOCK_DGRAM, "udp", 2, 2},
        { PJ_SOCK_DGRAM, "udp", 2, 4},
        { PJ_SOCK_DGRAM, "udp", 2, 8},
        { PJ_SOCK_DGRAM, "udp", 4, 1},
        { PJ_SOCK_DGRAM, "udp", 4, 2},
        { PJ_SOCK_DGRAM, "udp", 4, 4},
        { PJ_SOCK_DGRAM, "udp", 4, 8},
        { PJ_SOCK_STREAM, "tcp", 1, 1},
        { PJ_SOCK_STREAM, "tcp", 1, 2},
        { PJ_SOCK_STREAM, "tcp", 1, 4},
        { PJ_SOCK_STREAM, "tcp", 1, 8},
        { PJ_SOCK_STREAM, "tcp", 2, 1},
        { PJ_SOCK_STREAM, "tcp", 2, 2},
        { PJ_SOCK_STREAM, "tcp", 2, 4},
        { PJ_SOCK_STREAM, "tcp", 2, 8},
        { PJ_SOCK_STREAM, "tcp", 4, 1},
        { PJ_SOCK_STREAM, "tcp", 4, 2},
        { PJ_SOCK_STREAM, "tcp", 4, 4},
        { PJ_SOCK_STREAM, "tcp", 4, 8},
    };
    pj_size_t best_bandwidth;
    int best_index = 0;

    PJ_LOG(3,(THIS_FILE, "   Benchmarking ioqueue:"));
    PJ_LOG(3,(THIS_FILE, "   ==============================================="));
    PJ_LOG(3,(THIS_FILE, "   Type  Threads  Skt.Pairs  Avg.Time    Bandwidth"));
    PJ_LOG(3,(THIS_FILE, "   ==============================================="));

    best_bandwidth = 0;
    for (i=0; i<sizeof(test_param)/sizeof(test_param[0]); ++i) {
        pj_size_t bandwidth;

        rc = perform_test(test_param[i].type, 
                          test_param[i].type_name,
                          test_param[i].thread_cnt, 
                          test_param[i].sockpair_cnt, 
                          BUF_SIZE, 
                          &bandwidth);
        if (rc != 0)
            return rc;

        if (bandwidth > best_bandwidth)
            best_bandwidth = bandwidth, best_index = i;

        /* Give it a rest before next test. */
        pj_thread_sleep(500);
    }

    PJ_LOG(3,(THIS_FILE, 
              "   Best: Type=%s Threads=%d, Skt.Pairs=%d, Bandwidth=%u KB/s",
              test_param[best_index].type_name,
              test_param[best_index].thread_cnt,
              test_param[best_index].sockpair_cnt,
              best_bandwidth));
    PJ_LOG(3,(THIS_FILE, "   (Note: packet size=%d, total errors=%u)", 
			 BUF_SIZE, last_error_counter));
    return 0;
}

#else
/* To prevent warning about "translation unit is empty"
 * when this test is disabled. 
 */
int dummy_uiq_perf_test;
#endif  /* INCLUDE_IOQUEUE_PERF_TEST */


