#include "../fd_quic.h"
#include "fd_quic_test_helpers.h"
#include "../../../util/rng/fd_rng.h"

#include "../../../util/fibre/fd_fibre.h"

#include <stdlib.h>

/* number of streams to send/receive */
#define NUM_STREAMS 1000

/* done flags */

int client_done = 0;
int server_done = 0;

/* received count */
ulong rcvd     = 0;
ulong tot_rcvd = 0;

/* fibres for client and server */

fd_fibre_t * client_fibre = NULL;
fd_fibre_t * server_fibre = NULL;

/* "net" fibre for dropping and pcapping */

fd_fibre_t * net_fibre = NULL;

struct net_fibre_args {
  fd_fibre_pipe_t * input;
  fd_fibre_pipe_t * release;
  float             thresh;
  int               dir; /* 0=client->server  1=server->client */
};
typedef struct net_fibre_args net_fibre_args_t;


int state           = 0;
int server_complete = 0;
int client_complete = 0;

extern uchar pkt_full[];
extern ulong pkt_full_sz;

uchar fail = 0;

void
my_stream_notify_cb( fd_quic_stream_t * stream, void * ctx, int type ) {
  (void)stream;
  (void)ctx;
  (void)type;
}

int
my_stream_rx_cb( fd_quic_conn_t * conn,
                 ulong            stream_id,
                 ulong            offset,
                 uchar const *    data,
                 ulong            data_sz,
                 int              fin ) {
  (void)conn; (void)stream_id; (void)fin;

  //FD_LOG_NOTICE(( "received data from peer.  stream_id: %lu  size: %lu offset: %lu\n",
  //              (ulong)stream->stream_id, data_sz, offset ));
  (void)offset;
  FD_LOG_HEXDUMP_DEBUG(( "received data", data, data_sz ));

  FD_LOG_DEBUG(( "recv ok" ));

  rcvd++;
  tot_rcvd++;

  if( tot_rcvd == NUM_STREAMS ) client_done = 1;
  return FD_QUIC_SUCCESS;
}


struct my_context {
  int server;
};
typedef struct my_context my_context_t;

static ulong conn_final_cnt;

void
my_cb_conn_final( fd_quic_conn_t * conn,
                  void *           context ) {
  (void)context;

  fd_quic_conn_t ** ppconn = (fd_quic_conn_t**)fd_quic_conn_get_context( conn );
  if( ppconn ) {
    //FD_LOG_NOTICE(( "my_cb_conn_final %p SUCCESS", (void*)*ppconn ));
    *ppconn = NULL;
  }

  conn_final_cnt++;
}

void
my_connection_new( fd_quic_conn_t * conn,
                   void *           vp_context ) {
  (void)vp_context;

  //FD_LOG_NOTICE(( "server handshake complete" ));

  server_complete = 1;

  (void)conn;
}

void
my_handshake_complete( fd_quic_conn_t * conn,
                       void *           vp_context ) {
  (void)vp_context;

  //FD_LOG_NOTICE(( "client handshake complete" ));

  client_complete = 1;

  (void)conn;
}

/* global "clock" */
ulong now = (ulong)1e18;

ulong test_clock( void * ctx ) {
  (void)ctx;
  return now;
}

long
test_fibre_clock(void) {
  return (long)now;
}


struct client_args {
  fd_quic_t * quic;
  fd_quic_t * server_quic;
};
typedef struct client_args client_args_t;

void
client_fibre_fn( void * vp_arg ) {
  client_args_t * args = (client_args_t*)vp_arg;

  fd_quic_t * quic = args->quic;

  fd_quic_conn_t *   conn   = NULL;
  fd_quic_stream_t * stream = NULL;

  uchar buf[] = "Hello World!";

  ulong period_ns = (ulong)1e6;
  ulong next_send = now + period_ns;
  ulong sent      = 0;

  while( !client_done ) {
    ulong next_wakeup = fd_quic_get_next_wakeup( quic );

    /* wake up at either next service or next send, whichever is sooner */
    fd_fibre_wait_until( (long)fd_ulong_min( next_wakeup, next_send ) );

    fd_quic_service( quic );

    if( !conn ) {
      rcvd = sent = 0;

      conn = fd_quic_connect( quic, 0U, 0, 0U, 0 );
      if( !conn ) {
        FD_LOG_WARNING(( "Client unable to obtain a connection. now: %lu", (ulong)now ));
        continue;
      }

      fd_quic_conn_set_context( conn, &conn );

      /* wait for connection handshake */
      while( conn && conn->state != FD_QUIC_CONN_STATE_ACTIVE ) {
        /* service client */
        fd_quic_service( quic );

        /* allow server to process */
        fd_fibre_wait_until( (long)fd_quic_get_next_wakeup( quic ) );
      }

      continue;
    }

    if( !stream ) {
      if( rcvd != sent ) {
        fd_quic_service( quic );
        fd_fibre_wait_until( (long)fd_quic_get_next_wakeup( quic ) );

        continue;
      }

      stream = fd_quic_conn_new_stream( conn );

      if( !stream ) {
        if( conn->state == FD_QUIC_CONN_STATE_ACTIVE ) {
          FD_LOG_WARNING(( "Client unable to obtain a stream. now: %lu", (ulong)now ));
          ulong live = next_wakeup + (ulong)1e9;
          do {
            next_wakeup = fd_quic_get_next_wakeup( quic );

            if( next_wakeup > live ) {
              live = next_wakeup + (ulong)next_wakeup;
              FD_LOG_WARNING(( "Client waiting for a stream time: %lu", (ulong)now ));
            }

            /* wake up at either next service or next send, whichever is sooner */
            fd_fibre_wait_until( (long)next_wakeup );

            fd_quic_service( quic );

            if( !conn ) break;

            stream = fd_quic_conn_new_stream( conn );
          } while( !stream );
          FD_LOG_WARNING(( "Client obtained a stream" ));
        }
        next_send = now + period_ns; /* ensure we make progress */
        continue;
      }
    }

    /* set next send time */
    next_send = now + period_ns;

    /* have a stream, so send */
    int rc = fd_quic_stream_send( stream, buf, sizeof(buf), 1 /* fin */ );

    if( rc == FD_QUIC_SUCCESS ) {
      /* successful - stream will begin closing */

      if( ++sent % 15 == 0 ) {
        /* wait for last sends to complete */
        /* TODO add callback for this */
        ulong timeout = now + (ulong)3e6;
        while( now < timeout ) {
          fd_quic_service( quic );

          /* allow server to process */
          fd_fibre_wait_until( (long)fd_quic_get_next_wakeup( quic ) );
        }

        fd_quic_conn_close( conn, 0 );
        sent = 0;

        /* wait for connection to be reaped
           (it's set to NULL in final callback */
        while( conn ) {
          fd_quic_service( quic );

          /* allow server to process */
          fd_fibre_wait_until( (long)fd_quic_get_next_wakeup( quic ) );
        }

        stream = NULL;

        continue;
      }

      /* ensure new stream used for next send */
      stream = fd_quic_conn_new_stream( conn );

      /* TODO close logic */

    } else {
      FD_LOG_WARNING(( "send failed" ));
    }
  }

  if( conn ) {
    fd_quic_conn_close( conn, 0 );

    /* keep servicing until connection closed */
    while( conn ) {
      fd_quic_service( quic );
      fd_fibre_yield();
    }
  }

  /* tell the server to shutdown */
  server_done = 1;
}


struct server_args {
  fd_quic_t * quic;
};
typedef struct server_args server_args_t;


void
server_fibre_fn( void * vp_arg ) {
  server_args_t * args = (server_args_t*)vp_arg;

  fd_quic_t * quic = args->quic;

  /* wake up at least every 1ms */
  ulong period_ns = (ulong)1e6;
  while( !server_done ) {
    fd_quic_service( quic );

    ulong next_wakeup = fd_quic_get_next_wakeup( quic );
    ulong next_period = now + period_ns;

    fd_fibre_wait_until( (long)fd_ulong_min( next_wakeup, next_period ) );
  }
}


int
main( int argc, char ** argv ) {

  fd_boot          ( &argc, &argv );
  fd_quic_test_boot( &argc, &argv );

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, 0U, 0UL ) );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz  = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",   NULL, "gigantic"                   );
  ulong        page_cnt  = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",  NULL, 2UL                          );
  ulong        numa_idx  = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",  NULL, fd_shmem_numa_idx( cpu_idx ) );

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  FD_LOG_NOTICE(( "Creating workspace (--page-cnt %lu, --page-sz %s, --numa-idx %lu)", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  fd_quic_limits_t const quic_limits = {
    .conn_cnt           = 10,
    .conn_id_cnt        = 10,
    .handshake_cnt      = 10,
    .stream_id_cnt      = 10,
    .stream_pool_cnt    = 512,
    .inflight_frame_cnt = 1024 * 10,
    .tx_buf_sz          = 1<<14
  };

  ulong quic_footprint = fd_quic_footprint( &quic_limits );
  FD_TEST( quic_footprint );
  FD_LOG_NOTICE(( "QUIC footprint: %lu bytes", quic_footprint ));

  FD_LOG_NOTICE(( "Creating server QUIC" ));
  fd_quic_t * server_quic = fd_quic_new_anonymous( wksp, &quic_limits, FD_QUIC_ROLE_SERVER, rng );
  FD_TEST( server_quic );

  FD_LOG_NOTICE(( "Creating client QUIC" ));
  fd_quic_t * client_quic = fd_quic_new_anonymous( wksp, &quic_limits, FD_QUIC_ROLE_CLIENT, rng );
  FD_TEST( client_quic );

  client_quic->cb.conn_hs_complete = my_handshake_complete;
  client_quic->cb.stream_rx        = my_stream_rx_cb;
  client_quic->cb.stream_notify    = my_stream_notify_cb;
  client_quic->cb.conn_final       = my_cb_conn_final;

  client_quic->cb.now     = test_clock;
  client_quic->cb.now_ctx = NULL;

  client_quic->config.initial_rx_max_stream_data = 1<<15;

  server_quic->cb.conn_new       = my_connection_new;
  server_quic->cb.stream_rx      = my_stream_rx_cb;
  server_quic->cb.stream_notify  = my_stream_notify_cb;
  server_quic->cb.conn_final     = my_cb_conn_final;

  server_quic->cb.now     = test_clock;
  server_quic->cb.now_ctx = NULL;

  server_quic->config.initial_rx_max_stream_data = 1<<15;

  FD_LOG_NOTICE(( "Creating virtual pair" ));
  fd_quic_virtual_pair_t vp;
  fd_quic_virtual_pair_init( &vp, /*a*/ client_quic, /*b*/ server_quic );

  FD_LOG_NOTICE(( "Attaching AIOs" ));
  fd_quic_netem_t mitm_client_to_server;
  fd_quic_netem_t mitm_server_to_client;
  fd_quic_netem_init( &mitm_client_to_server, 0.01f, 0.01f );
  fd_quic_netem_init( &mitm_server_to_client, 0.01f, 0.01f );

  fd_quic_set_aio_net_tx( client_quic, &mitm_client_to_server.local );
  mitm_client_to_server.dst = vp.aio_a2b;
  fd_quic_set_aio_net_tx( server_quic, &mitm_server_to_client.local );
  mitm_server_to_client.dst = vp.aio_b2a;

  FD_LOG_NOTICE(( "Initializing QUICs" ));
  FD_TEST( fd_quic_init( client_quic ) );
  FD_TEST( fd_quic_init( server_quic ) );

  /* initialize fibres */
  void * this_fibre_mem = fd_wksp_alloc_laddr( wksp, fd_fibre_init_align(), fd_fibre_init_footprint( ), 1UL );
  fd_fibre_t * this_fibre = fd_fibre_init( this_fibre_mem ); (void)this_fibre;

  /* set fibre scheduler clock */
  fd_fibre_set_clock( test_fibre_clock );

  /* create fibres for client and server */
  ulong stack_sz = 1<<20;
  void * client_mem = fd_wksp_alloc_laddr( wksp, fd_fibre_start_align(), fd_fibre_start_footprint( stack_sz ), 1UL );
  client_args_t client_args[1] = {{ .quic = client_quic, .server_quic = server_quic }};
  client_fibre = fd_fibre_start( client_mem, stack_sz, client_fibre_fn, client_args );
  FD_TEST( client_fibre );

  void * server_mem = fd_wksp_alloc_laddr( wksp, fd_fibre_start_align(), fd_fibre_start_footprint( stack_sz ), 1UL );
  server_args_t server_args[1] = {{ .quic = server_quic }};
  server_fibre = fd_fibre_start( server_mem, stack_sz, server_fibre_fn, server_args );
  FD_TEST( server_fibre );

  /* schedule the fibres
     they will execute during the call to fibre_schedule_run */
  fd_fibre_schedule( client_fibre );
  fd_fibre_schedule( server_fibre );

  /* run the fibres until done */
  while(1) {
    long timeout = fd_fibre_schedule_run();
    if( timeout < 0 ) break;

    now = (ulong)timeout;
  }

  FD_LOG_NOTICE(( "Received %lu stream frags", tot_rcvd ));
  FD_LOG_NOTICE(( "Tested %lu connections", conn_final_cnt ));

  FD_LOG_NOTICE(( "Cleaning up" ));
  fd_quic_virtual_pair_fini( &vp );
  fd_wksp_free_laddr( fd_quic_delete( fd_quic_leave( server_quic ) ) );
  fd_wksp_free_laddr( fd_quic_delete( fd_quic_leave( client_quic ) ) );
  fd_wksp_delete_anonymous( wksp );
  fd_rng_delete( fd_rng_leave( rng ) );

  FD_LOG_NOTICE(( "pass" ));
  fd_quic_test_halt();
  fd_halt();
  return 0;
}
