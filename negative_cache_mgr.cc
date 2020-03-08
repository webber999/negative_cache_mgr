#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstring>

#include "ts/ts.h"
#include "ts/remap.h"
#include "tscore/ink_defs.h"
#include "tscore/ink_memory.h"

#define PLUGIN_NAME "negative_cache_mgr"
#define DEBUG_LOG(fmt, ...) TSDebug(PLUGIN_NAME, "[%s:%d] %s(): " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define ERROR_LOG(fmt, ...) TSError("[%s:%d] %s(): " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define ASSERT_SUCCESS(_x) TSAssert((_x) == TS_SUCCESS)
#define TS_NULL_MUTEX      NULL
#define NegativeStatusCodeNumber 12


static const int negativeStatusCode[NegativeStatusCodeNumber] = {
  TS_HTTP_STATUS_NO_CONTENT,
  TS_HTTP_STATUS_USE_PROXY,
  TS_HTTP_STATUS_BAD_REQUEST,
  TS_HTTP_STATUS_FORBIDDEN,
  TS_HTTP_STATUS_NOT_FOUND,
  TS_HTTP_STATUS_METHOD_NOT_ALLOWED,
  TS_HTTP_STATUS_REQUEST_URI_TOO_LONG,
  TS_HTTP_STATUS_INTERNAL_SERVER_ERROR,
  TS_HTTP_STATUS_NOT_IMPLEMENTED,
  TS_HTTP_STATUS_BAD_GATEWAY,
  TS_HTTP_STATUS_SERVICE_UNAVAILABLE,
  TS_HTTP_STATUS_GATEWAY_TIMEOUT,
};

struct NegativeCacheInfo
{
    int negative_cache_enable[NegativeStatusCodeNumber];
    int negative_cache_time[NegativeStatusCodeNumber];
};

static void server_resp_status_checker(TSHttpTxn txnp, struct NegativeCacheInfo *);
static void handle_server_read_response(TSHttpTxn, struct NegativeCacheInfo *);
static void handle_client_send_response(TSHttpTxn, struct NegativeCacheInfo *);
static int transaction_handler(TSCont, TSEvent, void *);
static bool set_header(TSMBuffer, TSMLoc, const char *, int, const char *, int);
static int args_handler(struct NegativeCacheInfo *, char *, int);

/**
 *  Read origin server response header and if status code is negative in NegativeCacheInfo,
 *  will cache the object according to negative_cache_time.
 *  
 */
static void 
server_resp_status_checker(TSHttpTxn txnp, NegativeCacheInfo *nc)
{
  TSCont txn_contp;
  if (NULL == (txn_contp = TSContCreate(transaction_handler, NULL))) {
    ERROR_LOG("failed to create the transaction handler continuation.");
  } else {
    TSContDataSet(txn_contp, nc);
    TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, txn_contp);
    TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, txn_contp);
    TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, txn_contp);
    DEBUG_LOG("Added TS_HTTP_READ_RESPONSE_HDR_HOOK, TS_HTTP_SEND_RESPONSE_HDR_HOOK, and TS_HTTP_TXN_CLOSE_HOOK");
  }
}

/**
 * Set a header to a specific value. This will avoid going to through a
 * remove / add sequence in case of an existing header.
 * but clean.
 *
 * From background_fetch.cc
 */
static bool
set_header(TSMBuffer bufp, TSMLoc hdr_loc, const char *header, int len, const char *val, int val_len)
{
  if (!bufp || !hdr_loc || !header || len <= 0 || !val || val_len <= 0) {
    return false;
  }

  DEBUG_LOG("header: %s, len: %d, val: %s, val_len: %d", header, len, val, val_len);
  bool ret         = false;
  TSMLoc field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, header, len);

  if (!field_loc) {
    // No existing header, so create one
    if (TS_SUCCESS == TSMimeHdrFieldCreateNamed(bufp, hdr_loc, header, len, &field_loc)) {
      if (TS_SUCCESS == TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1, val, val_len)) {
        TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        ret = true;
      }
      TSHandleMLocRelease(bufp, hdr_loc, field_loc);
    }
  } else {
    TSMLoc tmp = NULL;
    bool first = true;

    while (field_loc) {
      if (first) {
        first = false;
        if (TS_SUCCESS == TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1, val, val_len)) {
          ret = true;
        }
      } else {
        TSMimeHdrFieldDestroy(bufp, hdr_loc, field_loc);
      }
      tmp = TSMimeHdrFieldNextDup(bufp, hdr_loc, field_loc);
      TSHandleMLocRelease(bufp, hdr_loc, field_loc);
      field_loc = tmp;
    }
  }

  return ret;
}

/**
 * After receiving a response from the origin, change
 * the response negative status code(4XX/5XX) to a 200 OK so that
 * the response will be written to cache.
 */
static void
handle_server_read_response(TSHttpTxn txnp, struct NegativeCacheInfo *nc)
{
  TSMBuffer response;
  TSMLoc resp_hdr;
  TSHttpStatus status;
  bool negative_cache = false;
  int negative_cache_time = 0;

  if (TS_SUCCESS == TSHttpTxnServerRespGet(txnp, &response, &resp_hdr)) {
    //only when status is in negative_cache_enable array，then process it.
    status = TSHttpHdrStatusGet(response, resp_hdr);
    
    for (int i = 0; i < NegativeStatusCodeNumber; ++i) {
      if ((int)status == nc->negative_cache_enable[i]) {
        negative_cache = true;
        negative_cache_time = nc->negative_cache_time[i];
        break;
      }
    }

    // negative status code response cache
    // convert negative status code to 200 temporarily, add add tag `tmp_cache_XXX`
    if (negative_cache) {
      char cache_tag[] = "tmp_cache_";
      char status_str[4];
      snprintf(status_str, 4, "%d", (int)status);
      strcat(cache_tag, status_str);

      char *temp_cc_header = (char *) TSmalloc(255 * sizeof (char));
      if (NULL == temp_cc_header) {
          DEBUG_LOG("TSmalloc failed");
          TSHandleMLocRelease(response, resp_hdr, NULL);
          return;
        }
      
      int len = snprintf(temp_cc_header, 255 * sizeof (char), "max-age=%d", negative_cache_time);

      if (set_header(response, resp_hdr, TS_MIME_FIELD_CACHE_CONTROL, TS_MIME_LEN_CACHE_CONTROL, temp_cc_header, len)) {
          DEBUG_LOG("add cache-control header max-age: %s", temp_cc_header);
      }
      
      TSHttpHdrStatusSet(response, resp_hdr, TS_HTTP_STATUS_OK);
      TSHttpHdrReasonSet(response, resp_hdr, cache_tag, 13);
      
      DEBUG_LOG("cache tag: %s", cache_tag);
      DEBUG_LOG("Get %s Response, cache body to TS_HTTP_STATUS_OK", status_str);
      
      TSfree(temp_cc_header);
      TSHandleMLocRelease(response, resp_hdr, NULL);
    }
  }
  TSHandleMLocRelease(response, resp_hdr, NULL);
}

/**
 * Changes the response code back to the negative status code before
 * replying to the client.
 */
static void
handle_client_send_response(TSHttpTxn txnp, struct NegativeCacheInfo *nc)
{
  char *p;
  int length;
  TSMBuffer response;
  TSMLoc resp_hdr;

  TSReturnCode result = TSHttpTxnClientRespGet(txnp, &response, &resp_hdr);
  if (TS_SUCCESS == result) {
    // DEBUG_LOG("TSHttpTxnClientRespGet result is true...");
    // a cached result will have a TS_HTTP_OK with a 'tmp_cache_XXX' reason
    if ((p = (char *)TSHttpHdrReasonGet(response, resp_hdr, &length)) != NULL) {
      if ((length == 13) && (0 == strncasecmp(p, "tmp_cache_", 10))) {
        char status_str[4];
        strncpy(status_str, p+10, 3);
        int i_status = atoi(status_str);
        
        DEBUG_LOG("status_string: %s", status_str);
        // convert i_status to enum TSHttpStatus
        TSHttpStatus status = (TSHttpStatus) i_status;

        DEBUG_LOG("tmp negative %d cache!!!", status);
        TSHttpHdrStatusSet(response, resp_hdr, status);
        TSHttpHdrReasonSet(response, resp_hdr, TSHttpHdrReasonLookup(status),
              strlen(TSHttpHdrReasonLookup(status)));
      }
    }
  }
  TSHandleMLocRelease(response, resp_hdr, NULL);
}


/**
 * Transaction event handler.
 */
static int
transaction_handler(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp            = static_cast<TSHttpTxn>(edata);
  struct NegativeCacheInfo *nc = (struct NegativeCacheInfo *)TSContDataGet(contp);

  switch (event) {
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    handle_server_read_response(txnp, nc);
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    handle_client_send_response(txnp, nc);
    break;
  case TS_EVENT_HTTP_TXN_CLOSE:
    // can not TSfree nc !!!
    TSContDestroy(contp);
    break;
  default:
    TSAssert(!"Unexpected event");
    break;
  }
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}


/**
 *  parsing arguments, convert string to integer array,
 *  and write to struct nc.
 */
static int 
args_handler(struct NegativeCacheInfo *nc, char *str, int struct_index)
{
  char *sub_str;
  int i=0; 
  sub_str = strtok(str, ";");
  
  DEBUG_LOG("arguments: %s", str);

  while (sub_str != NULL) {
    if (struct_index == 0) {
      nc->negative_cache_enable[i] = atoi(sub_str);
    } else if (struct_index == 1) {
      nc->negative_cache_time[i] = atoi(sub_str);
    }
    sub_str = strtok(NULL, ";");
    i++;
  }
  return i;
}

/**
 *  init env variable
 */
TSReturnCode
TSRemapNewInstance(int argc, char *argv[], void **ih, char * /*errbuf */, int /* errbuf_size */)
{
  struct NegativeCacheInfo *nc = (struct NegativeCacheInfo *)TSmalloc(sizeof(NegativeCacheInfo));
  memset(nc, 0, sizeof(NegativeCacheInfo));

  char negative_cache_enable_str[64];
  char negative_cache_time_str[128];
  int negative_cache_num;

  if (argc <= 2) {
    *ih = (void *)nc;
    return TS_SUCCESS;
  }

  for (int i=2; i < argc; i++) {
    if (strncasecmp(argv[i], "--negative_cache_enable=", 24) == 0) {
      strcpy(negative_cache_enable_str, argv[i]+24);
      negative_cache_num = args_handler(nc, negative_cache_enable_str, 0);
    } 
    else if (strncasecmp(argv[i], "--negative_cache_time=", 22) == 0) {
      strcpy(negative_cache_time_str, argv[i]+22);
      if (negative_cache_num != args_handler(nc, negative_cache_time_str, 1)) {
        ERROR_LOG("arguments is invalid!!!");
      };
    }
    else {
      ERROR_LOG("the argument `%s` is not be recognized.", argv[i]);
    }
  }

  // check negative_cache_enable array is invalid ?
  for (int i = 0; i < negative_cache_num; i++) {
    bool invalid_status = true;
    for (int j=0; j<NegativeStatusCodeNumber; j++) {
      if (negativeStatusCode[j] == nc->negative_cache_enable[i]) {
        break;
      }
    }
    if (!invalid_status) {
      ERROR_LOG("invalid negative status code: %d", nc->negative_cache_enable[i]);
      return TS_ERROR;
    }
    if (nc->negative_cache_enable[i] == 0) {
      break;
    }
    DEBUG_LOG("negative cache code: %d", nc->negative_cache_enable[i]);
    DEBUG_LOG("negative cache time: %d", nc->negative_cache_time[i]);
  }

  *ih = (void *)nc;
  return TS_SUCCESS;
}

/**
 * `traffic_ctl config reload` will trigger this func.
 */
void
TSRemapDeleteInstance(void *ih)
{
  NegativeCacheInfo *nc = (NegativeCacheInfo *)ih;

  if (nc != NULL) {
    DEBUG_LOG("delete Instance: negative_cache_enable");
    TSfree(nc);
  }
}

/**
 * Remap entry point.
 */
TSRemapStatus
TSRemapDoRemap(void *ih, TSHttpTxn txnp, TSRemapRequestInfo * /* rri */)
{
  NegativeCacheInfo *nc = (NegativeCacheInfo *)ih;
  DEBUG_LOG("start do remap");
  server_resp_status_checker(txnp, nc);
  return TSREMAP_NO_REMAP;
}

TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
  if (!api_info) {
    strncpy(errbuf, "[tsremap_init] - Invalid TSRemapInterface argument", errbuf_size - 1);
    return TS_ERROR;
  }

  if (api_info->tsremap_version < TSREMAP_VERSION) {
    snprintf(errbuf, errbuf_size, "[TSRemapInit] - Incorrect API version %ld.%ld", api_info->tsremap_version >> 16,
             (api_info->tsremap_version & 0xffff));
    return TS_ERROR;
  }
  DEBUG_LOG("==========initialized==========");
  return TS_SUCCESS;
}

