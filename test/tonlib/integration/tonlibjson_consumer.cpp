#include "tonlib/tonlib_client_json.h"
#include "tonlib/tonlib_engine_console.h"

int main() {
  void* client = tonlib_client_json_create();
  tonlib_client_json_destroy(client);

  TonlibEventLoop* loop = tonlib_event_loop_create(1);
  tonlib_event_loop_destroy(loop);

  return 0;
}
