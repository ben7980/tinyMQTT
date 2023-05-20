//
// Created by zr on 23-4-9.
//
#include "mqtt_broker.h"
#include "net/mqtt_tcp_conn.h"
#include "base/mqtt_util.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static void remove_tcp_conn(tmq_tcp_conn_t* conn, void* arg)
{
    tmq_io_group_t* group = conn->group;
    tcp_conn_ctx* ctx = conn->context;
    assert(ctx != NULL);

    tmq_event_loop_cancel_timer(&group->loop, ctx->timerid);

    char conn_name[50];
    tmq_tcp_conn_id(conn, conn_name, sizeof(conn_name));
    tmq_map_erase(group->tcp_conns, conn_name);

    tlog_info("remove connection [%s]", conn_name);
}

static void handle_timeout(void* arg)
{
    tmq_tcp_conn_t* conn = (tmq_tcp_conn_t*) arg;
    tcp_conn_ctx* ctx = conn->context;
    assert(ctx != NULL);

    ctx->ttl--;
    if(ctx->ttl <= 0)
    {
        char conn_name[50];
        tmq_tcp_conn_id(conn, conn_name, sizeof(conn_name));
        tlog_info("connection timeout [%s]", conn_name);
        tmq_tcp_conn_destroy(conn);
    }
}

static void handle_new_connection(void* arg)
{
    tmq_io_group_t* group = arg;

    pthread_mutex_lock(&group->pending_conns_lk);
    tmq_vec(tmq_socket_t) conns = tmq_vec_make(tmq_socket_t);
    tmq_vec_swap(&conns, &group->pending_conns);
    pthread_mutex_unlock(&group->pending_conns_lk);

    for(tmq_socket_t* it = tmq_vec_begin(conns); it != tmq_vec_end(conns); it++)
    {
        tmq_tcp_conn_t* conn = tmq_tcp_conn_new(group, *it, &group->broker->codec);
        conn->close_cb = remove_tcp_conn;

        tmq_timer_t* timer = tmq_timer_new(MQTT_ALIVE_TIMER_INTERVAL, 1, handle_timeout, conn);
        tmq_timerid_t timerid = tmq_event_loop_add_timer(&group->loop, timer);

        tcp_conn_ctx* conn_ctx = malloc(sizeof(tcp_conn_ctx));
        conn_ctx->context = group->broker;
        conn_ctx->in_session = 0;
        conn_ctx->ttl = MQTT_CONNECT_PENDING;
        conn_ctx->timerid = timerid;
        tmq_tcp_conn_set_context(conn, conn_ctx);

        char conn_name[50];
        tmq_tcp_conn_id(conn, conn_name, sizeof(conn_name));
        tmq_map_put(group->tcp_conns, conn_name, conn);

        tlog_info("new connection [%s] group=%p thread=%lu", conn_name, group, mqtt_tid);
    }
    tmq_vec_free(conns);
}

static void tmq_io_group_init(tmq_io_group_t* group, tmq_broker_t* broker)
{
    group->broker = broker;
    tmq_event_loop_init(&group->loop);
    tmq_map_str_init(&group->tcp_conns, tmq_tcp_conn_t*, MAP_DEFAULT_CAP, MAP_DEFAULT_LOAD_FACTOR);
    tmq_vec_init(&group->pending_conns, tmq_socket_t);
    tmq_notifier_init(&group->new_conn_notifier, &group->loop, handle_new_connection, group);
    pthread_mutex_init(&group->pending_conns_lk, NULL);
}

static void* io_group_thread_func(void* arg)
{
    tmq_io_group_t* group = (tmq_io_group_t*) arg;
    tmq_event_loop_run(&group->loop);
    tmq_event_loop_destroy(&group->loop);
}

static void tmq_io_group_run(tmq_io_group_t* group)
{
    if(pthread_create(&group->io_thread, NULL, io_group_thread_func, group) != 0)
        fatal_error("pthread_create() error %d: %s", errno, strerror(errno));
}

static void dispatch_new_connection(tmq_socket_t conn, void* arg)
{
    tmq_broker_t* broker = (tmq_broker_t*) arg;
    tmq_io_group_t* next_group = &broker->io_groups[broker->next_io_group++];
    if(broker->next_io_group >= MQTT_IO_THREAD)
        broker->next_io_group = 0;

    pthread_mutex_lock(&next_group->pending_conns_lk);
    tmq_vec_push_back(next_group->pending_conns, conn);
    pthread_mutex_unlock(&next_group->pending_conns_lk);

    tmq_notifier_notify(&next_group->new_conn_notifier);
}

void tmq_broker_init(tmq_broker_t* broker, uint16_t port)
{
    if(!broker) return;
    tmq_event_loop_init(&broker->event_loop);
    tmq_codec_init(&broker->codec);

    tmq_acceptor_init(&broker->acceptor, &broker->event_loop, port);
    tmq_acceptor_set_cb(&broker->acceptor, dispatch_new_connection, broker);

    for(int i = 0; i < MQTT_IO_THREAD; i++)
        tmq_io_group_init(&broker->io_groups[i], broker);
    broker->next_io_group = 0;
}

void tmq_broker_run(tmq_broker_t* broker)
{
    if(!broker) return;
    for(int i = 0; i < MQTT_IO_THREAD; i++)
        tmq_io_group_run(&broker->io_groups[i]);
    tmq_acceptor_listen(&broker->acceptor);
    tmq_event_loop_run(&broker->event_loop);

    tmq_event_loop_destroy(&broker->event_loop);

}