#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include "ioserv.h"

void http_result_write(struct http_result *slot, const char *data, char reason)
{
  pthread_mutex_lock(&slot->lock);
  if (!slot->cancelled) {
    slot->reason = reason;
    if (data) {
      strncpy(slot->data, data, sizeof slot->data - 1);
      slot->data[sizeof slot->data - 1] = 0;
      slot->state = HTTP_DONE;
    } else {
      slot->data[0] = 0;
      slot->state = HTTP_ERROR;
    }
  }
  pthread_mutex_unlock(&slot->lock);
}

void http_result_cancel(struct http_result *slot)
{
  pthread_mutex_lock(&slot->lock);
  slot->cancelled = 1;
  slot->state = HTTP_IDLE;
  pthread_mutex_unlock(&slot->lock);
}

void http_result_clear_cancelled(struct http_result *slot)
{
  pthread_mutex_lock(&slot->lock);
  slot->cancelled = 0;
  slot->state = HTTP_IDLE;
  pthread_mutex_unlock(&slot->lock);
}
