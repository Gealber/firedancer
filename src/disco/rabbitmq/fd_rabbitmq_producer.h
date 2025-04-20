#ifndef HEADER_fd_src_disco_rabbitmq_fd_rabbitmq_producer_h
#define HEADER_fd_src_disco_rabbitmq_fd_rabbitmq_producer_h

// NOTE: This is not part of the original firedancer implementation
#include "../../util/log/fd_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>

struct fd_rabbitmq_clt {
    amqp_connection_state_t conn; 
    char const *queue_name;
    char const *exchange;
};

typedef struct fd_rabbitmq_clt fd_rabbitmq_clt_t;

fd_rabbitmq_clt_t init_rclt(const char *hostname, int port);
void close_rclt(fd_rabbitmq_clt_t clt);
static int rabbitmq_publish(fd_rabbitmq_clt_t clt, unsigned char *msg, size_t msg_len);

#endif /* HEADER_fd_src_disco_rabbitmq_fd_rabbitmq_producer_h */
