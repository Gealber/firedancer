#include "fd_rabbitmq_producer.h"

void die_on_amqp_error(amqp_rpc_reply_t x, char const *context) {
  switch (x.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      return;

    case AMQP_RESPONSE_NONE:
      FD_LOG_ERR(("%s: missing RPC reply type!\n", context));
      break;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      FD_LOG_ERR(("%s: %s\n", context, amqp_error_string2(x.library_error)));
      break;

    case AMQP_RESPONSE_SERVER_EXCEPTION:
      switch (x.reply.id) {
        case AMQP_CONNECTION_CLOSE_METHOD: {
          amqp_connection_close_t *m =
              (amqp_connection_close_t *)x.reply.decoded;
          FD_LOG_ERR(("%s: server connection error %uh, message: %.*s\n",
                  context, m->reply_code, (int)m->reply_text.len,
                  (char *)m->reply_text.bytes));
          break;
        }
        case AMQP_CHANNEL_CLOSE_METHOD: {
          amqp_channel_close_t *m = (amqp_channel_close_t *)x.reply.decoded;
          FD_LOG_ERR(("%s: server channel error %uh, message: %.*s\n",
                  context, m->reply_code, (int)m->reply_text.len,
                  (char *)m->reply_text.bytes));
          break;
        }
        default:
          FD_LOG_ERR(("%s: unknown server error, method id 0x%08X\n",
                  context, x.reply.id));
          break;
      }
      break;
  }

  // exit(1);
}

void die_on_error(int x, char const *context) {
  if (x < 0) {
    FD_LOG_ERR(("%s: %s\n", context, amqp_error_string2(x)));
    // exit(1);
  }
}

fd_rabbitmq_clt_t init_rclt(char *hostname, int port, const char *username, const char *password) {
  int ret;
  amqp_socket_t *socket = NULL;
  amqp_connection_state_t conn;
  fd_rabbitmq_clt_t clt;

  if (hostname[0] == '\0') 
    hostname = "127.0.0.1";
  if (port == 0)
    port = 5672;

  conn = amqp_new_connection();
  socket = amqp_tcp_socket_new(conn);
  if (!socket) {
      FD_LOG_ERR(("creating TCP socket"));
      return clt;
  }

  FD_LOG_INFO(("opening rabbitmq socket to connection: hostname: %s port: %d", hostname, port));
  ret = amqp_socket_open(socket, hostname, port);
  if (ret) {
      FD_LOG_ERR(("opening TCP socket"));
      return clt;
  }

  FD_LOG_INFO(("logging with username: %s password: %s", username, password));
  die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, username, password), "Logging in");

  FD_LOG_INFO(("opening channel rabbitmq"));
  amqp_channel_open(conn, 1);
  FD_LOG_INFO(("opening channel rabbitmq"));
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");

  clt.conn = conn;
  clt.exchange = "shreds";
  clt.queue_name = "shreds";

  FD_LOG_INFO(("successfull rabbitmq initialization"));
  return clt;
}

void close_rclt(fd_rabbitmq_clt_t clt) {
    die_on_amqp_error(amqp_channel_close(clt.conn, 1, AMQP_REPLY_SUCCESS),
                    "Closing channel");
    die_on_amqp_error(amqp_connection_close(clt.conn, AMQP_REPLY_SUCCESS),
                    "Closing connection");    
    die_on_error(amqp_destroy_connection(clt.conn), "Ending connection");
}

// publish a message in a rabbitmq queue returns negative in case of error. In case of error
// make use of amqp_error_string2 to get the error string.
int rabbitmq_publish(fd_rabbitmq_clt_t clt, unsigned char *msg, size_t msg_len) {
    amqp_bytes_t message_bytes = {
        .bytes = msg,
        .len = msg_len
    };

    return amqp_basic_publish(
      clt.conn, 
      1, 
      amqp_cstring_bytes(clt.exchange),
      amqp_cstring_bytes(clt.queue_name), 0, 0, NULL,
      message_bytes);
}