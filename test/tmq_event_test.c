//
// Created by zr on 23-4-18.
//
#include "event/mqtt_event.h"
#include "net/mqtt_acceptor.h"
#include "tlog.h"
#include <unistd.h>
#include <stdio.h>

void new_conn(tmq_socket_t conn, void* arg)
{
    printf("new_conn\n");
    tmq_event_loop_t* loop = (tmq_event_loop_t*)arg;
    tmq_event_loop_quit(loop);
}

int main()
{
    tlog_init("broker.log", 1024 * 1024, 10, 0, TLOG_SCREEN);

    tmq_event_loop_t loop;
    tmq_event_loop_init(&loop);

    tmq_acceptor_t acceptor;
    tmq_acceptor_init(&acceptor, &loop, 9999);
    tmq_acceptor_set_cb(&acceptor, new_conn, &loop);
    tmq_acceptor_listen(&acceptor);

    tmq_event_loop_run(&loop);

    tmq_event_loop_destroy(&loop);

    tlog_exit();
    return 0;
}