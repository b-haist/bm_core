#include "bm_service.h"
#include "bm_config.h"
#include "bm_os.h"
#include "bm_service_common.h"
#include "bm_service_request.h"
#include "string.h"

#define DefaultServiceRequestTimeoutMs 100

typedef struct BmServiceListElem {
  const char *service;
  size_t service_strlen;
  BmServiceHandler service_handler;
  struct BmServiceListElem *next;
} BmServiceListElem;

typedef struct BmServiceContext {
  BmServiceListElem *service_list;
  BmSemaphore lock;
} BmServiceContext;

static BmServiceContext BM_SERVICE_CONTEXT;

static void _service_list_add_service(BmServiceListElem *list_elem);
static bool _service_list_remove_service(const char *service,
                                         size_t service_strlen);
static BmServiceListElem *
_service_create_list_elem(size_t service_strlen, const char *service,
                          BmServiceHandler service_handler);
static bool _service_sub_unsub_to_req_topic(size_t service_strlen,
                                            const char *service, bool sub);
static void _service_request_received_cb(uint64_t node_id, const char *topic,
                                         uint16_t topic_len,
                                         const uint8_t *data, uint16_t data_len,
                                         uint8_t type, uint8_t version);

/*!
 * @brief Register a service.
 * @param[in] service_strlen The length of the service string.
 * @param[in] service The service string.
 * @param[in] service_handler The callback to call when a request is received.
 * @return True if the service was registered, false otherwise.
 */
bool bm_service_register(size_t service_strlen, const char *service,
                         BmServiceHandler service_handler) {
  bool rval = false;
  if (service && service_handler) {
    if (bm_semaphore_take(BM_SERVICE_CONTEXT.lock,
                          DefaultServiceRequestTimeoutMs) == BmOK) {
      do {
        BmServiceListElem *list_elem =
            _service_create_list_elem(service_strlen, service, service_handler);
        if (!list_elem) {
          break;
        }
        _service_list_add_service(list_elem);
        if (!_service_sub_unsub_to_req_topic(service_strlen, service, true)) {
          break;
        }
        rval = true;
      } while (0);
      bm_semaphore_give(BM_SERVICE_CONTEXT.lock);
    }
  }
  return rval;
}

/*!
 * @brief Unregister a service.
 * @param[in] service_strlen The length of the service string.
 * @param[in] service The service string.
 * @return True if the service was unregistered, false otherwise.
 */
bool bm_service_unregister(size_t service_strlen, const char *service) {
  bool rval = false;
  if (service) {
    if (bm_semaphore_take(BM_SERVICE_CONTEXT.lock,
                          DefaultServiceRequestTimeoutMs) == BmOK) {
      do {
        if (!_service_sub_unsub_to_req_topic(service_strlen, service, false)) {
          break;
        }
        if (!_service_list_remove_service(service, service_strlen)) {
          break;
        }
        rval = true;
      } while (0);
      bm_semaphore_give(BM_SERVICE_CONTEXT.lock);
    }
  }
  return rval;
}
/*!
 * @brief Initialize the service module.
 * @note Will initialize both the service request and service reply modules.
 */
BmErr bm_service_init(void) {
  BmErr err = BmENOMEM;
  BM_SERVICE_CONTEXT.lock = bm_semaphore_create();
  if (BM_SERVICE_CONTEXT.lock) {
    err = bm_service_request_init();
  }
  return err;
}

static void _service_list_add_service(BmServiceListElem *list_elem) {
  if (list_elem) {
    if (BM_SERVICE_CONTEXT.service_list == NULL) {
      BM_SERVICE_CONTEXT.service_list = list_elem;
    } else {
      BmServiceListElem *current = BM_SERVICE_CONTEXT.service_list;
      while (current->next != NULL) {
        current = current->next;
      }
      current->next = list_elem;
    }
  }
}

static BmServiceListElem *
_service_create_list_elem(size_t service_strlen, const char *service,
                          BmServiceHandler service_handler) {
  BmServiceListElem *list_elem =
      (BmServiceListElem *)(bm_malloc(sizeof(BmServiceListElem)));
  if (list_elem) {
    list_elem->service = service;
    list_elem->service_strlen = service_strlen;
    list_elem->service_handler = service_handler;
    list_elem->next = NULL;
  }
  return list_elem;
}

static bool _service_sub_unsub_to_req_topic(size_t service_strlen,
                                            const char *service, bool sub) {
  bool rval = false;
  char *topic_str = NULL;
  do {
    size_t topic_strlen = service_strlen + strlen(BM_SERVICE_REQ_STR);
    topic_str = (char *)(bm_malloc(topic_strlen));
    if (topic_str) {
      memcpy(topic_str, service, service_strlen);
      memcpy(topic_str + service_strlen, BM_SERVICE_REQ_STR,
             strlen(BM_SERVICE_REQ_STR));
      if (sub) {
        if (bm_sub_wl(topic_str, topic_strlen, _service_request_received_cb) !=
            BmOK) {
          break;
        }
      } else {
        if (bm_unsub_wl(topic_str, topic_strlen,
                        _service_request_received_cb) != BmOK) {
          break;
        }
      }
      rval = true;
    }
  } while (0);
  if (topic_str) {
    bm_free(topic_str);
  }
  return rval;
}

static void _service_request_received_cb(uint64_t node_id, const char *topic,
                                         uint16_t topic_len,
                                         const uint8_t *data, uint16_t data_len,
                                         uint8_t type, uint8_t version) {
  (void)type;
  (void)version;

  if (bm_semaphore_take(BM_SERVICE_CONTEXT.lock,
                        DefaultServiceRequestTimeoutMs) == BmOK) {
    BmServiceListElem *current = BM_SERVICE_CONTEXT.service_list;
    while (current != NULL) {
      if (strncmp(current->service, topic, current->service_strlen) == 0) {
        BmServiceRequestDataHeader *request_header =
            (BmServiceRequestDataHeader *)data;
        if (data_len !=
            sizeof(BmServiceRequestDataHeader) + request_header->data_size) {
          bm_debug("Request data length does not match header.\n");
          break;
        }
        if (topic_len != current->service_strlen + strlen(BM_SERVICE_REQ_STR)) {
          bm_debug("Topic length does not match service length.\n");
          break;
        }
        // We found the service, make a reply buffer, and call the handler to create a reply.
        size_t reply_len =
            MAX_BM_SERVICE_DATA_SIZE -
            sizeof(BmServiceReplyDataHeader); // Max size of reply data
        uint8_t *reply_data = (uint8_t *)(bm_malloc(MAX_BM_SERVICE_DATA_SIZE));
        BmServiceReplyDataHeader *reply_header =
            (BmServiceReplyDataHeader *)reply_data;
        memset(reply_data, 0, MAX_BM_SERVICE_DATA_SIZE);
        if (current->service_handler(current->service_strlen, current->service,
                                     request_header->data_size,
                                     request_header->data, &reply_len,
                                     reply_header->data)) {
          reply_header->target_node_id = node_id;
          reply_header->id = request_header->id;
          reply_header->data_size =
              reply_len; // On return from the handler, reply_len is now the actual size of the reply data

          // Publish the reply
          size_t pub_topic_len =
              current->service_strlen + strlen(BM_SERVICE_REP_STR);
          char *pub_topic = (char *)(bm_malloc(pub_topic_len));
          if (pub_topic) {
            memcpy(pub_topic, current->service, current->service_strlen);
            memcpy(pub_topic + current->service_strlen, BM_SERVICE_REP_STR,
                   strlen(BM_SERVICE_REP_STR));
            bm_pub_wl(pub_topic, pub_topic_len, reply_data,
                      reply_len + sizeof(BmServiceReplyDataHeader), 0,
                      BM_COMMON_PUB_SUB_VERSION);
            bm_free(pub_topic);
          }
        }

        // Free the allocated memory
        bm_free(reply_data);
        break;
      }
      current = current->next;
    }
    bm_semaphore_give(BM_SERVICE_CONTEXT.lock);
  }
}

static bool _service_list_remove_service(const char *service,
                                         size_t service_strlen) {
  bool rval = false;
  if (service) {
    BmServiceListElem *current = BM_SERVICE_CONTEXT.service_list;
    BmServiceListElem *prev = NULL;
    while (current != NULL) {
      if (strncmp(current->service, service, service_strlen) == 0) {
        if (prev == NULL) {
          BM_SERVICE_CONTEXT.service_list = current->next;
        } else {
          prev->next = current->next;
        }
        bm_free(current);
        rval = true;
        break;
      }
      prev = current;
      current = current->next;
    }
  }
  return rval;
}
