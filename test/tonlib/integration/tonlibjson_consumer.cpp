#include "tonlib/tonlib_client_json.h"

int main() {
  void* client = tonlib_client_json_create();
  tonlib_client_json_destroy(client);

  return 0;
}
