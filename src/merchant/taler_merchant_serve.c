/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file merchant/taler_merchant_serve.c
 * @brief Reference implementation of the merchant's HTTP interface
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */


#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include <taler/taler_util.h>
#include "merchant.h"
#include "merchant_db.h"
#include <taler/taler_mint_service.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>


/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)

/**
 * Shorthand for exit jumps due to protocol exceptions resulting from client's
 * mistakes
 */
#define EXITIF_OP(cond)                                              \
  do {                                                               \
    if (cond) { GNUNET_break_op (0); goto EXITIF_exit; }             \
  } while (0)

/**
 * Print JSON parsing related error information
 */
#define WARN_JSON(error)                                                \
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,                              \
                "JSON parsing failed at %s:%u: %s (%s)",                \
                __FILE__, __LINE__, error.text, error.source)

/**
 * Macro to round microseconds to seconds in GNUNET_TIME_* structs.
 */
#define ROUND_TO_SECS(name,us_field) name.us_field -= name.us_field % (1000 * 1000)


struct ContractData
{
  char *product;
};


GNUNET_NETWORK_STRUCT_BEGIN

struct Contract
{
  /**
   * The signature of the merchant for this contract
   */
  struct GNUNET_CRYPTO_EddsaSignature sig;

  /**
   * Purpose header for the signature over contract
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * The transaction identifier
   */
  char m[13];

  /**
   * Expiry time
   */
  struct GNUNET_TIME_AbsoluteNBO t;

  /**
   * The invoice amount
   */
  struct TALER_AmountNBO amount;

  /**
   * The hash of the preferred wire format + nounce
   */
  struct GNUNET_HashCode h_wire;

  /**
   * The contract data
   */
  char a[];
};

GNUNET_NETWORK_STRUCT_END

/**
 * A download object
 */
struct Download {
  struct Download *next;
  struct Download *prev;
  char *filename;
  struct MHD_Response *resp;
  unsigned int id;
};

/**
 * DLL for downloadable objects
 */
struct Download *dwn_head;
struct Download *dwn_tail;

/**
 * MHD response object for listing all products
 */
struct MHD_Response *list_products_resp;

/**
 * Number of files we make available for downloading
 */
static unsigned int ndownloads;


/**
 * Context information of the mints we trust
 */
struct Mint
{
  /**
   * Public key of this mint
   */
  struct GNUNET_CRYPTO_EddsaPublicKey pubkey;

  /**
   * Connection handle to this mint
   */
  struct TALER_MINT_Handle *conn;
};

/**
 * Hashmap to store the mint context information
 */
static struct GNUNET_CONTAINER_MultiPeerMap *mints_map;

/**
 * Our private key
 */
struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;

/**
 * Connection handle to the our database
 */
PGconn *db_conn;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * Our wireformat
 */
static struct MERCHANT_WIREFORMAT_Sepa *wire;

/**
 * Hash of the wireformat
 */
static struct GNUNET_HashCode h_wire;

/**
 * Shutdown task identifier
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Task for calling the select on MHD's sockets
 */
static struct GNUNET_SCHEDULER_Task *select_task;

/**
 * The port we are running on
 */
static long long unsigned port;

/**
 * Mint context
 */
static struct TALER_MINT_Context *mctx;

/**
 * Our hostname
 */
static char *hostname;

/**
 * Directory of data items to serve
 */
static char *data_dir;

/**
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;



/**
 * Send JSON object as response.  Decreases the reference count of the
 * JSON object.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param status_code the http status code
 * @return MHD result code
 */
static int
send_response_json (struct MHD_Connection *connection,
                    json_t *json,
                    unsigned int status_code)
{
  struct MHD_Response *resp;
  char *json_str;

  json_str = json_dumps (json, JSON_INDENT(2));
  json_decref (json);
  resp = MHD_create_response_from_buffer (strlen (json_str), json_str,
                                          MHD_RESPMEM_MUST_FREE);
  if (NULL == resp)
    return MHD_NO;
  return MHD_queue_response (connection, status_code, resp);
}


/* ************ JSON post-processing logic; FIXME: why do we use JSON here!? ********** */


/**
 * Initial size for POST
 * request buffer.
 */
#define REQUEST_BUFFER_INITIAL 1024

/**
 * Maximum POST request size
 */
#define REQUEST_BUFFER_MAX (1024*1024)


/**
 * Buffer for POST requests.
 */
struct Buffer
{
  /**
   * Allocated memory
   */
  char *data;

  /**
   * Number of valid bytes in buffer.
   */
  size_t fill;

  /**
   * Number of allocated bytes in buffer.
   */
  size_t alloc;
};


/**
 * Initialize a buffer.
 *
 * @param buf the buffer to initialize
 * @param data the initial data
 * @param data_size size of the initial data
 * @param alloc_size size of the buffer
 * @param max_size maximum size that the buffer can grow to
 * @return a GNUnet result code
 */
static int
buffer_init (struct Buffer *buf, const void *data, size_t data_size, size_t alloc_size, size_t max_size)
{
  if (data_size > max_size || alloc_size > max_size)
    return GNUNET_SYSERR;
  if (data_size > alloc_size)
    alloc_size = data_size;
  buf->data = GNUNET_malloc (alloc_size);
  memcpy (buf->data, data, data_size);
  return GNUNET_OK;
}


/**
 * Free the data in a buffer.  Does *not* free
 * the buffer object itself.
 *
 * @param buf buffer to de-initialize
 */
static void
buffer_deinit (struct Buffer *buf)
{
  GNUNET_free (buf->data);
  buf->data = NULL;
}


/**
 * Append data to a buffer, growing the buffer if necessary.
 *
 * @param buf the buffer to append to
 * @param data the data to append
 * @param size the size of @a data
 * @param max_size maximum size that the buffer can grow to
 * @return GNUNET_OK on success,
 *         GNUNET_NO if the buffer can't accomodate for the new data
 *         GNUNET_SYSERR on fatal error (out of memory?)
 */
static int
buffer_append (struct Buffer *buf, const void *data, size_t data_size, size_t max_size)
{
  if (buf->fill + data_size > max_size)
    return GNUNET_NO;
  if (data_size + buf->fill > buf->alloc)
  {
    char *new_buf;
    size_t new_size = buf->alloc;
    while (new_size < buf->fill + data_size)
      new_size += 2;
    if (new_size > max_size)
      return GNUNET_NO;
    new_buf = GNUNET_malloc (new_size);
    memcpy (new_buf, buf->data, buf->fill);
    buf->data = new_buf;
    buf->alloc = new_size;
  }
  memcpy (buf->data + buf->fill, data, data_size);
  buf->fill += data_size;
  return GNUNET_OK;
}



/**
 * Process a POST request containing a JSON object.
 *
 * @param connection the MHD connection
 * @param con_cs the closure (contains a 'struct Buffer *')
 * @param upload_data the POST data
 * @param upload_data_size the POST data size
 * @param json the JSON object for a completed request
 *
 * @returns
 *    GNUNET_YES if json object was parsed
 *    GNUNET_NO is request incomplete or invalid
 *    GNUNET_SYSERR on internal error
 */
static int
process_post_json (struct MHD_Connection *connection,
                   void **con_cls,
                   const char *upload_data,
                   size_t *upload_data_size,
                   json_t **json)
{
  struct Buffer *r = *con_cls;

  if (NULL == *con_cls)
  {
    /* We are seeing a fresh POST request. */

    r = GNUNET_new (struct Buffer);
    if (GNUNET_OK != buffer_init (r, upload_data, *upload_data_size,
                 REQUEST_BUFFER_INITIAL, REQUEST_BUFFER_MAX))
    {
      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return GNUNET_SYSERR;
    }
    *upload_data_size = 0;
    *con_cls = r;
    return GNUNET_NO;
  }
  if (0 != *upload_data_size)
  {
    /* We are seeing an old request with more data available. */

    if (GNUNET_OK != buffer_append (r, upload_data, *upload_data_size,
                                    REQUEST_BUFFER_MAX))
    {
      /* Request too long or we're out of memory. */

      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return GNUNET_SYSERR;
    }
    *upload_data_size = 0;
    return GNUNET_NO;
  }

  /* We have seen the whole request. */

  *json = json_loadb (r->data, r->fill, 0, NULL);
  buffer_deinit (r);
  GNUNET_free (r);
  if (NULL == *json)
  {
    struct MHD_Response *resp;
    int ret;

    GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Can't parse JSON request body\n");
    resp = MHD_create_response_from_buffer (strlen ("parse error"),
                                            "parse error",
                                            MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection,
                              MHD_HTTP_BAD_REQUEST,
                              resp);
    MHD_destroy_response (resp);
    return ret;
  }
  *con_cls = NULL;

  return GNUNET_YES;
}


/* ************** END of JSON POST processing logic ************ */


static struct GNUNET_HashCode
hash_wireformat (uint64_t nounce)
{
  struct GNUNET_HashContext *hc;
  struct GNUNET_HashCode hash;

  hc = GNUNET_CRYPTO_hash_context_start ();
  GNUNET_CRYPTO_hash_context_read (hc, wire->iban, strlen (wire->iban));
  GNUNET_CRYPTO_hash_context_read (hc, wire->name, strlen (wire->name));
  GNUNET_CRYPTO_hash_context_read (hc, wire->bic, strlen (wire->bic));
  nounce = GNUNET_htonll (nounce);
  GNUNET_CRYPTO_hash_context_read (hc, &nounce, sizeof (nounce));
  GNUNET_CRYPTO_hash_context_finish (hc, &hash);
  return hash;
}


static json_t *
build_json_contract (struct Contract *contract)
{
  return json_pack ("{s:s, s:o, s:o, s:s, s:o, s:o}",
                   "transaction_id", contract->m,
                   "expiry", TALER_JSON_from_abs (GNUNET_TIME_absolute_ntoh (contract->t)),
                    "amount", TALER_JSON_from_amount (TALER_amount_ntoh (contract->amount)),
                   "description", contract->a,
                   "H_wire", TALER_JSON_from_data (&contract->h_wire, sizeof (struct GNUNET_HashCode)),
                   "msig", TALER_JSON_from_data (&contract->sig, sizeof (struct GNUNET_CRYPTO_EddsaSignature)));
}

/**
 * Cleeanup entries in the peer map.
 *
 * @param cls closure
 * @param key current public key
 * @param value value in the hash map
 * @return #GNUNET_YES if we should continue to
 *         iterate,
 *         #GNUNET_NO if not.
 */
static int
mints_cleanup_iterator (void *cls,
                        const struct GNUNET_PeerIdentity *key,
                        void *value)
{
  struct Mint *mint = value;

  if (NULL != mint->conn)
    TALER_MINT_disconnect (mint->conn);
  GNUNET_CONTAINER_multipeermap_remove (mints_map, key, mint);
  GNUNET_free (mint);
  return GNUNET_YES;
}


/**
 * Shutdown task
 *
 * @param cls NULL
 * @param tc scheduler task context
 */
static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct Download *dwn;

  shutdown_task = NULL;
  if (NULL != select_task)
  {
    GNUNET_SCHEDULER_cancel (select_task);
    select_task = NULL;
  }
  if (NULL != list_products_resp)
  {
    MHD_destroy_response (list_products_resp);
    list_products_resp = NULL;
  }
  if (NULL != mhd)
  {
    MHD_stop_daemon (mhd);
    mhd = NULL;
  }
  if (NULL != db_conn)
  {
    MERCHANT_DB_disconnect (db_conn);
    db_conn = NULL;
  }
  if (NULL != mints_map)
  {
    GNUNET_CONTAINER_multipeermap_iterate (mints_map,
                                           &mints_cleanup_iterator,
                                           NULL);
    GNUNET_CONTAINER_multipeermap_destroy (mints_map);
    mints_map = NULL;
  }
  if (NULL != mctx)
  {
    TALER_MINT_cleanup (mctx);
    mctx = NULL;
  }
  if (NULL != wire)
  {
    TALER_MERCHANT_destroy_wireformat_sepa (wire);
    wire = NULL;
  }
  while (NULL != (dwn = dwn_head))
  {
    GNUNET_CONTAINER_DLL_remove (dwn_head, dwn_tail, dwn);
    if (NULL != dwn->resp)
      MHD_destroy_response (dwn->resp);
    GNUNET_free (dwn->filename);
    GNUNET_free (dwn);
  }
}


/**
 * Get the MHD's sockets which are to be called with select() and schedule the
 * select task.
 *
 * @return GNUNET_YES upon success; GNUNET_NO upon error, in this case the
 *           select task will not be queued.
 */
static int
poll_mhd ();


/**
 * One of the MHD's sockets are ready.  Call MHD_run_from_select ().
 *
 * @param cls NULL
 * @param tc scheduler task context
 */
static void
run_mhd (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  fd_set fd_rs;
  fd_set fd_ws;
  select_task = NULL;
  if (0 != (GNUNET_SCHEDULER_REASON_SHUTDOWN & tc->reason))
    return;
  FD_ZERO (&fd_rs);
  FD_ZERO (&fd_ws);
  if (0 != (GNUNET_SCHEDULER_REASON_READ_READY & tc->reason))
    fd_rs = tc->read_ready->sds;
  if (0 != (GNUNET_SCHEDULER_REASON_WRITE_READY & tc->reason))
    fd_ws = tc->write_ready->sds;
  EXITIF (MHD_YES != MHD_run_from_select (mhd,
                                          &fd_rs,
                                          &fd_ws,
                                          NULL));
  EXITIF (GNUNET_NO == poll_mhd ());
  return;

 EXITIF_exit:
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Get the MHD's sockets which are to be called with select() and schedule the
 * select task.
 *
 * @return GNUNET_YES upon success; GNUNET_NO upon error, in this case the
 *           select task will not be queued.
 */
static int
poll_mhd ()
{
  struct GNUNET_NETWORK_FDSet rs;
  struct GNUNET_NETWORK_FDSet ws;
  fd_set fd_rs;
  fd_set fd_ws;
  fd_set fd_es;
  struct GNUNET_TIME_Relative delay;
  unsigned long long timeout;
  int max_fd;

  FD_ZERO (&fd_rs);
  FD_ZERO (&fd_ws);
  FD_ZERO (&fd_es);
  max_fd = 0;
  if (MHD_YES != MHD_get_fdset (mhd,
                                &fd_rs,
                                &fd_ws,
                                &fd_es,
                                &max_fd))
    return GNUNET_SYSERR;
  GNUNET_NETWORK_fdset_zero (&rs);
  GNUNET_NETWORK_fdset_zero (&ws);
  GNUNET_NETWORK_fdset_copy_native (&rs, &fd_rs, max_fd + 1);
  GNUNET_NETWORK_fdset_copy_native (&ws, &fd_ws, max_fd + 1);
  if (MHD_NO == MHD_get_timeout (mhd, &timeout))
    timeout = 0;
  if (0 == timeout)
    delay = GNUNET_TIME_UNIT_FOREVER_REL;
  else
    delay = GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS,
                                           timeout);
  if (NULL != select_task)
    GNUNET_SCHEDULER_cancel (select_task);
  select_task = GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_KEEP,
                                             delay,
                                             &rs,
                                             &ws,
                                             &run_mhd,
                                             NULL);
  return GNUNET_OK;
}

static int
failure_resp (struct MHD_Connection *connection, unsigned int status)
{
  static char page_404[]="\
<!DOCTYPE html>                                         \
<html><title>Resource not found</title><body><center>   \
<h3>The resource you are looking for is not found.</h3> \
</center></body></html>";
  static char page_500[]="\
<!DOCTYPE html> <html><title>Internal Server Error</title><body><center> \
<h3>The server experienced an internal error and hence cannot serve your \
request</h3></center></body></html>";
  struct MHD_Response *resp;
  char *page;
  size_t size;
#define PAGE(number) \
  do {page=page_ ## number; size=sizeof(page_ ## number)-1;} while(0)

  GNUNET_assert (400 <= status);
  resp = NULL;
  switch (status)
  {
  case 404:
    PAGE(404);
    break;
  default:
    status = 500;
  case 500:
    PAGE(500);
  }
#undef PAGE

  EXITIF (NULL == (resp = MHD_create_response_from_buffer (size,
                                                           page,
                                                           MHD_RESPMEM_PERSISTENT)));
  EXITIF (MHD_YES != MHD_queue_response (connection, status, resp));
  MHD_destroy_response (resp);
  return GNUNET_OK;

 EXITIF_exit:
  if (NULL != resp)
    MHD_destroy_response (resp);
  return GNUNET_SYSERR;
}


/**
 * Iterator over key-value pairs.  This iterator
 * can be used to iterate over all of the cookies,
 * headers, or POST-data fields of a request, and
 * also to iterate over the headers that have been
 * added to a response.
 *
 * @param cls closure
 * @param kind kind of the header we are looking at
 * @param key key for the value, can be an empty string
 * @param value corresponding value, can be NULL
 * @return #MHD_YES to continue iterating,
 *         #MHD_NO to abort the iteration
 * @ingroup request
 */
static int
get_contract_values_iter (void *cls,
                          enum MHD_ValueKind kind,
                          const char *key, const char *value)
{
  unsigned long long id;
  uint64_t *product = cls;

#define STR_PRODUCT "product"
  if (0 == strncasecmp (key, STR_PRODUCT, sizeof (STR_PRODUCT) - 1))
  {
    if (1 > sscanf (value, "%llu", &id))
      return GNUNET_NO;
    *product = (uint64_t) id;
  }
  return GNUNET_YES;
}

#if 0
static const char *
uint64_to_enc (uint64_t i)
{
  static char buf[14];
  i = GNUNET_htonll (i);
  GNUNET_break (NULL !=
                GNUNET_STRINGS_data_to_string (&i, sizeof (i), buf, sizeof (buf)));
  buf[13] = '\0';
  return buf;
}

static uint64_t
enc_to_uint64 (const char *enc)
{
  uint64_t i;
  GNUNET_break (GNUNET_OK ==
                GNUNET_STRINGS_string_to_data (enc, strlen(enc), &i, sizeof
                (i)));
  return GNUNET_ntohll (i);
}
#endif

/**
 * Prepare a contract, store it in database and send the corresponding JSON.
 *
 * @param connection MHD connection handle
 * @param _resp pointer to hold the result response upon success
 * @return the status code 200 when a contract is generated; 404 when the
 *           product is not found or upon other errors.
 */
static unsigned int
handle_get_contract (struct MHD_Connection *connection,
                     struct MHD_Response **_resp)
{
  struct MHD_Response *resp;
  struct Contract *contract;
  struct GNUNET_TIME_Absolute expiry;
  struct TALER_Amount amount;
  char *template = "A contract from GNUnet e.V thanking you for a"
      " donation of the aforementioned amount.  As a token of gratitude, upon"
      " successful payment, you may download your image at "
      "`http://%s:%u/download?ref=[]'";
  char *desc;
  json_t *json;
  char *json_str;
  uint64_t nounce;
  uint64_t product;
  uint64_t contract_id_nbo;
  long long contract_id;
  unsigned int ret;

  resp = NULL;
  contract = NULL;
  desc = NULL;
  ret = 400;
  product = UINT64_MAX;
  MHD_get_connection_values (connection, MHD_GET_ARGUMENT_KIND,
                             &get_contract_values_iter, &product);
  if (UINT64_MAX == product)
    goto EXITIF_exit;

  expiry = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get (),
                                     GNUNET_TIME_UNIT_DAYS);
  ROUND_TO_SECS (expiry, abs_value_us);
  amount.value = 1;
  amount.fraction = 0;
  strcpy (amount.currency, "EUR");
  nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);

  /* Prepare contract */
  (void) GNUNET_asprintf (&desc,
                          template,
                          hostname,
                          port);
  contract_id = MERCHANT_DB_contract_create (db_conn,
                                    expiry,
                                    &amount,
                                    desc,
                                    nounce,
                                    product);
  EXITIF (-1 == contract_id);
  contract_id_nbo = GNUNET_htonll ((uint64_t) contract_id);
  contract = GNUNET_malloc (sizeof (struct Contract) + strlen (desc) + 1);
  contract->purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract->purpose.size = htonl (sizeof (struct Contract)
                                 - offsetof (struct Contract, purpose)
                                 + strlen (desc) + 1);
  GNUNET_STRINGS_data_to_string (&contract_id_nbo, sizeof (contract_id_nbo),
                                 contract->m, sizeof (contract->m));
  contract->t = GNUNET_TIME_absolute_hton (expiry);
  (void) strcpy (contract->a, desc);
  contract->h_wire = hash_wireformat (nounce);
  contract->amount = TALER_amount_hton (amount);
  GNUNET_CRYPTO_eddsa_sign (privkey, &contract->purpose, &contract->sig);
  json = build_json_contract (contract);
  json_str = json_dumps (json, JSON_INDENT(2));
  json_decref (json);
  resp = MHD_create_response_from_buffer (strlen (json_str), json_str,
                                          MHD_RESPMEM_MUST_FREE);
  ret = 200;

 EXITIF_exit:
  GNUNET_free_non_null (desc);
  if (NULL != resp)
    *_resp = resp;
  if (NULL != contract)
  {
    GNUNET_free (contract);
  }
  return ret;
}

static struct Download *
find_product (unsigned int id)
{
  struct Download *dwn;

  for (dwn = dwn_head; NULL != dwn; dwn = dwn->next)
  {
    if (dwn->id == id)
      return dwn;
  }
  return NULL;
}


static int
get_download_ref (void *cls,
                 enum MHD_ValueKind kind,
                 const char *key, const char *value)
{
  char **coin_pub_enc = cls;

  if (0 == strncasecmp (key, "ref", sizeof ("ref")))
  {
    *coin_pub_enc = GNUNET_strdup (value);
    return MHD_NO;
  }
  return MHD_YES;
}

static unsigned int
handle_download (struct MHD_Connection *conn,
                 struct MHD_Response **_resp)
{
  char *coin_pub_enc;
  struct Download *item;
  struct GNUNET_DISK_FileHandle *fh;
  struct GNUNET_CRYPTO_EddsaPublicKey coin_pub;
  long long product_id;
  off_t size;
  int ret;

  coin_pub_enc = NULL;
  ret = MHD_HTTP_NOT_FOUND;
  MHD_get_connection_values (conn, MHD_GET_ARGUMENT_KIND,
                             &get_download_ref, &coin_pub_enc);
  LOG_WARNING ("Trying to start downloading with coin: %s\n", coin_pub_enc);
  EXITIF (NULL == coin_pub_enc);
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CRYPTO_eddsa_public_key_from_string (coin_pub_enc,
                                                      strlen (coin_pub_enc),
                                                      &coin_pub));
  product_id = MERCHANT_DB_get_checkout_product (db_conn,
                                                 &coin_pub);
  EXITIF (-1 == product_id);
  EXITIF (NULL == (item = find_product ((unsigned int) product_id)));
  if (NULL != item->resp)
  {
    *_resp = item->resp;
    ret = MHD_HTTP_OK;
    goto EXITIF_exit;
  }
  fh = GNUNET_DISK_file_open (item->filename,
                              GNUNET_DISK_OPEN_READ,
                              GNUNET_DISK_PERM_USER_READ);
  GNUNET_assert (NULL != fh);
  GNUNET_assert (GNUNET_OK == GNUNET_DISK_file_handle_size (fh,
                                                            &size));
  item->resp = MHD_create_response_from_fd (size, fh->fd);
  GNUNET_assert (MHD_NO != MHD_add_response_header (item->resp,
                                                    "Content-Type",
                                                    "image/jpeg"));
  GNUNET_free (fh);

 EXITIF_exit:
  GNUNET_free_non_null (coin_pub_enc);
  return ret;
}

struct CheckoutCtx
{
  /* FIXME: Hook into a DLL for cleaner shutdown */
  struct MHD_Connection *conn;
  struct TALER_MINT_DepositHandle *dh;
  struct Download *product;
  char *coin_pub_enc;
  uint64_t transaction_id;
  struct GNUNET_CRYPTO_EddsaPublicKey coin_pub;
  struct GNUNET_CRYPTO_EddsaSignature coin_sig;
  struct TALER_Amount amount;
  struct GNUNET_SCHEDULER_Task *timeout_task;

};


/**
 * Callbacks of this type are used to serve the result of submitting a deposit
 * permission object to a mint
 *
 * @param cls closure
 * @param status 1 for successful deposit, 2 for retry, 0 for failure
 * @param obj the received JSON object; can be NULL if it cannot be constructed
 *        from the reply
 * @param emsg in case of unsuccessful deposit, this contains a human readable
 *        explanation.
 */
static void
checkout_status (void *cls, int status, json_t *obj, char *emsg)
{
  struct CheckoutCtx *ctx = cls;
  const char *tmplt_download_page =
      "<!DOCTYPE HTML><html>"
      "<body>You are being redirected to the product download page<br>"
      "If your browser is unable to redirect, you may click "
      "<a href=\"%s\">here</a> to download.</body>"
      "</html>";
  char *download_page;
  char *location;
  struct MHD_Response *resp;
  int size;

  LOG_DEBUG ("Processing checkout request reply\n");
  GNUNET_SCHEDULER_cancel (ctx->timeout_task);
  ctx->timeout_task = NULL;
  download_page = NULL;
  location = NULL;
  switch (status)
  {
  case 1:
    {
      struct GNUNET_CRYPTO_EddsaPublicKey coin_pub;

      GNUNET_assert (GNUNET_SYSERR !=
                     GNUNET_CRYPTO_eddsa_public_key_from_string
                     (ctx->coin_pub_enc,
                      strlen (ctx->coin_pub_enc),
                      &coin_pub));
      /* FIXME: Put the contract into the checkout DB. */
    }
    /* redirect with HTTP FOUND 302 to the product download page */
    GNUNET_assert (NULL != obj);
    GNUNET_assert (0 < (size = GNUNET_asprintf (&location,
                                                "/download?ref=%s",
                                                ctx->coin_pub_enc)));
    GNUNET_assert (0 < (size = GNUNET_asprintf (&download_page,
                                                tmplt_download_page,
                                                location)));
    resp = MHD_create_response_from_buffer (size,
                                            download_page,
                                            MHD_RESPMEM_MUST_FREE);
    /* IMP: do not free `download_page' */
    GNUNET_assert (NULL != resp);
    GNUNET_assert (MHD_NO != MHD_add_response_header (resp,
                                                      "Location",
                                                      location));
    GNUNET_assert (MHD_YES == MHD_queue_response (ctx->conn,
                                                  MHD_HTTP_FOUND,
                                                  resp));
    MHD_destroy_response (resp);
    GNUNET_free (location);
    location = NULL;
    resp = NULL;
#if 0
    struct Download *product;
    struct GNUNET_DISK_FileHandle *fh;
    GNUNET_assert (NULL != (product = ctx->product));
    if (NULL != product->resp)
    {
      MHD_queue_response (ctx->conn, MHD_HTTP_OK, product->resp);
      break;
    }
    fh = GNUNET_DISK_file_open (product->filename,
                                GNUNET_DISK_OPEN_READ,
                                GNUNET_DISK_PERM_USER_READ);
    GNUNET_assert (NULL != fh);
    GNUNET_assert (GNUNET_OK == GNUNET_DISK_file_handle_size (fh,
                                                              &size));
    product->resp = MHD_create_response_from_fd (size, fh->fd);
    GNUNET_assert (MHD_NO != MHD_add_response_header (product->resp,
                                                      "Content-Type",
                                                      "image/jpeg"));
    GNUNET_free (fh);
    MHD_queue_response (ctx->conn, MHD_HTTP_OK, product->resp);
#endif

    break;
  case 2:
    send_response_json (ctx->conn,
                        json_pack ("{s:s}", "status", "pending"),
                        200);   /* FIXME: Send Image data */
    break;
  case 0:
    send_response_json (ctx->conn,
                        json_pack ("{s:s s:s}",
                                   "status", "failed",
                                   "error", (NULL != emsg) ? emsg : "unknown"),
                        400);     /* FIXME */
    break;
  default:
    GNUNET_assert (0);          /* should never reach */
  }
  GNUNET_free (ctx->coin_pub_enc);
  GNUNET_free (ctx);
  if (GNUNET_SYSERR == poll_mhd ())
  {
    GNUNET_break (0);
    GNUNET_SCHEDULER_shutdown ();
  }
}

static void
checkout_status_timedout (void *cls,
                          const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct CheckoutCtx *ctx = cls;

  LOG_DEBUG ("Checkout request timed out\n");
  ctx->timeout_task = NULL;
  TALER_MINT_deposit_submit_cancel (ctx->dh);
  ctx->dh = NULL;
  send_response_json (ctx->conn,
                      json_pack ("{s:s}", "error", "timeout"),
                      400);     /* FIXME */
  GNUNET_free (ctx->coin_pub_enc);
  GNUNET_free (ctx);
  EXITIF (GNUNET_SYSERR == poll_mhd ());
  return;

 EXITIF_exit:
  GNUNET_SCHEDULER_shutdown ();
}

static int
handle_checkout (struct MHD_Connection *conn,
                 json_t *checkout_json)
{
  struct CheckoutCtx *ctx;
  const char *pkey_enc;
  const char *tid_enc;
  const char *emsg;
  const char *coin_pub_enc;
  const char *coin_sig_enc;
  struct Mint *mint;
  struct Download *product;
  struct GNUNET_CRYPTO_EddsaPublicKey pkey;
  struct GNUNET_CRYPTO_EddsaPublicKey coin_pub;
  struct GNUNET_CRYPTO_EddsaSignature coin_sig;
  uint64_t tid;
  uint64_t product_id;
  json_error_t jerror;
  unsigned int status;

  coin_pub_enc = NULL;
  emsg = "Public key of Mint is missing in the request";
  status = MHD_HTTP_BAD_REQUEST;
  if (-1 == json_unpack_ex (checkout_json,
                            &jerror,
                            0,
                            "{s:s s:s s:s s:s}",
                            "mint_pub", &pkey_enc,
                            "transaction_id", &tid_enc,
                            "coin_pub", &coin_pub_enc,
                            "coin_sig", &coin_sig_enc))
  {
    WARN_JSON (jerror);
    goto EXITIF_exit;
  }

  EXITIF (GNUNET_OK != GNUNET_STRINGS_string_to_data
          (tid_enc, strlen (tid_enc), &tid, sizeof (tid)));
  tid = GNUNET_ntohll (tid);

  emsg = "Public key of the coin is missing/malformed in the request";
  EXITIF (NULL == coin_pub_enc);
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CRYPTO_eddsa_public_key_from_string (coin_pub_enc,
                                                      strlen (coin_pub_enc),
                                                      &coin_pub));

  emsg = "Signature of the coin is missing/malformed in the request";
  EXITIF (NULL == coin_sig_enc);
  EXITIF (GNUNET_SYSERR ==
          GNUNET_STRINGS_string_to_data (coin_sig_enc, strlen (coin_sig_enc),
                                         &coin_sig, sizeof (coin_sig)));

  emsg = "Contract not found";
  status = MHD_HTTP_NOT_FOUND;
  LOG_DEBUG ("Looking for product associated with transaction %u\n", tid);
  EXITIF (-1 == (product_id = MERCHANT_DB_get_contract_product (db_conn, tid)));

  emsg = "Could not find the downloadable product.  Sorry :(";
  EXITIF (NULL == (product = find_product (product_id)));

  emsg = "Invalid public key given for a mint";
  EXITIF (52 != strlen (pkey_enc));
  EXITIF (GNUNET_SYSERR == GNUNET_STRINGS_string_to_data (pkey_enc, 52,
                                                          &pkey, sizeof (pkey)));

  emsg = "The provided mint is not trusted by us";
  status = MHD_HTTP_FORBIDDEN;
  EXITIF (NULL == (mint =
                   GNUNET_CONTAINER_multipeermap_get (mints_map,
                                                      (const struct
                                                       GNUNET_PeerIdentity *)
                                                      &pkey)));

  LOG_DEBUG ("Creating a new checkout request\n");
  ctx = GNUNET_new (struct CheckoutCtx);
  ctx->product = product;
  ctx->conn = conn;
  ctx->coin_pub_enc = GNUNET_strdup (coin_pub_enc);
  ctx->transaction_id = tid;
  ctx->coin_pub = coin_pub;
  ctx->coin_sig = coin_sig;
  /* FIXME: parse amount */
  /* ctx->amount = ?? */
  ctx->dh = TALER_MINT_deposit_submit_json (mint->conn,
                                            checkout_status,
                                            ctx,
                                            checkout_json);
  ctx->timeout_task = GNUNET_SCHEDULER_add_delayed
      (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 3),
       &checkout_status_timedout, ctx);
  return MHD_YES;

 EXITIF_exit:
  json_decref (checkout_json);
  return send_response_json (conn,
                             json_pack ("{s:s s:s}",
                                        "status", "failed",
                                        "error", emsg),
                             status);
}

/**
 * A client has requested the given url using the given method
 * (#MHD_HTTP_METHOD_GET, #MHD_HTTP_METHOD_PUT,
 * #MHD_HTTP_METHOD_DELETE, #MHD_HTTP_METHOD_POST, etc).  The callback
 * must call MHD callbacks to provide content to give back to the
 * client and return an HTTP status code (i.e. #MHD_HTTP_OK,
 * #MHD_HTTP_NOT_FOUND, etc.).
 *
 * @param cls argument given together with the function
 *        pointer when the handler was registered with MHD
 * @param url the requested url
 * @param method the HTTP method used (#MHD_HTTP_METHOD_GET,
 *        #MHD_HTTP_METHOD_PUT, etc.)
 * @param version the HTTP version string (i.e.
 *        #MHD_HTTP_VERSION_1_1)
 * @param upload_data the data being uploaded (excluding HEADERS,
 *        for a POST that fits into memory and that is encoded
 *        with a supported encoding, the POST data will NOT be
 *        given in upload_data and is instead available as
 *        part of #MHD_get_connection_values; very large POST
 *        data *will* be made available incrementally in
 *        @a upload_data)
 * @param upload_data_size set initially to the size of the
 *        @a upload_data provided; the method must update this
 *        value to the number of bytes NOT processed;
 * @param con_cls pointer that the callback can set to some
 *        address and that will be preserved by MHD for future
 *        calls for this request; since the access handler may
 *        be called many times (i.e., for a PUT/POST operation
 *        with plenty of upload data) this allows the application
 *        to easily associate some request-specific state.
 *        If necessary, this state can be cleaned up in the
 *        global #MHD_RequestCompletedCallback (which
 *        can be set with the #MHD_OPTION_NOTIFY_COMPLETED).
 *        Initially, `*con_cls` will be NULL.
 * @return #MHD_YES if the connection was handled successfully,
 *         #MHD_NO if the socket must be closed due to a serios
 *         error while handling the request
 */
static int
url_handler (void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **con_cls)
{
#define URL_PRODUCTS "/products"
#define URL_CONTRACT "/contract"
#define URL_CHECKOUT "/checkout"
#define URL_HTTPTEST "/httptest"
#define URL_DOWNLOAD "/download"
#define STR_404_NOTFOUND "The requested resource is not found"
  struct MHD_Response *resp;
  int no_destroy;
  unsigned int status;

  resp = NULL;
  status = 404;
  no_destroy = 0;
  LOG_DEBUG ("request for URL `%s'\n", url);

  if (0 == strncasecmp (url, URL_PRODUCTS, sizeof (URL_PRODUCTS)))
  {
    /* parse for /contract */
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
    {
      resp = list_products_resp;
      no_destroy = 1;
      status = 200;
    }
    else
      GNUNET_break (0);         /* FIXME: implement for post */
  }

  if (0 == strncasecmp (url, URL_CONTRACT, sizeof (URL_CONTRACT)))
  {
    /* parse for /contract */
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
      status = handle_get_contract (connection, &resp);
    else
      GNUNET_break (0);         /* FIXME: implement for post */
  }

  if (0 == strncasecmp (url, URL_CHECKOUT, sizeof (URL_CHECKOUT)))
  {
    json_t *checkout_obj;
    int ret;
    /* parse for /checkout */
    ret = process_post_json (connection,
                             con_cls,
                             upload_data,
                             upload_data_size,
                             &checkout_obj);
    if (GNUNET_SYSERR == ret)
      return MHD_NO;
    if (GNUNET_NO == ret)
      return MHD_YES;
    /* Handle the response in the request handler */
    ret = handle_checkout (connection, checkout_obj);
    return ret;
  }

  if (0 == strncasecmp (url, URL_HTTPTEST, sizeof (URL_HTTPTEST)))
  {
    static char page[]="\
<!DOCTYPE html> \
<html><title>HTTP Test page</title><body><center><h3>HTTP Test page</h3> \
</center></body></html>";
    resp = MHD_create_response_from_buffer (sizeof (page) - 1,
                                            page,
                                            MHD_RESPMEM_PERSISTENT);
    EXITIF (NULL == resp);
  }

  if ((0 == strncasecmp (url, URL_DOWNLOAD, sizeof (URL_DOWNLOAD)))
      && (0 == strcmp (MHD_HTTP_METHOD_GET, method)))
  {
    status = handle_download (connection, &resp);
    if (status != MHD_HTTP_OK)
      no_destroy = 1;
  }
  if (NULL != resp)
  {
    EXITIF (MHD_YES != MHD_queue_response (connection, status, resp));
    if (!no_destroy)
      MHD_destroy_response (resp);
  }
  else
    EXITIF (GNUNET_OK != failure_resp (connection, status));
  return MHD_YES;

 EXITIF_exit:
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
  return MHD_NO;
}


/**
 * Callback for catching serious error conditions from MHD.
 *
 * @param cls user specified value
 * @param file where the error occured
 * @param line where the error occured
 * @param reason error detail, may be NULL
 */
static void
mhd_panic_cb (void *cls,
              const char *file,
              unsigned int line,
              const char *reason)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "MHD panicked at %s:%u: %s",
              file, line, reason);
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}

/**
 * Function called with a filename.
 *
 * @param cls closure
 * @param filename complete filename (absolute path)
 * @return #GNUNET_OK to continue to iterate,
 *  #GNUNET_NO to stop iteration with no error,
 *  #GNUNET_SYSERR to abort iteration with error!
 */
static int
add_download_file (void *cls, const char *filename)
{
  struct Download *dwn;

  dwn = GNUNET_new (struct Download);
  dwn->filename = GNUNET_strdup (filename);
  dwn->id = ndownloads++;
  GNUNET_CONTAINER_DLL_insert (dwn_head, dwn_tail, dwn);
  return GNUNET_OK;
}


/**
 * Function to build a MHD response object to list products
 *
 * @return GNUNET_OK upon success; GNUNET_SYSERR otherwise
 */
static int
build_list_product_response ()
{
  const char *header = "\
<!DOCTYPE html> \
<html><title>Products List</title> \
<body><center><ol>";
  char **partials;
  const char *footer = "</ol></center></body></html>";
  char *page;
  struct Download *dwn;
  size_t size;
  unsigned int cnt;
  int psize;
  unsigned int header_size;
  unsigned int footer_size;
  unsigned int *partial_sizes;
  int ret;

  ret = GNUNET_SYSERR;
  GNUNET_assert (NULL == list_products_resp);
  header_size = strlen (header);
  footer_size = strlen (footer);
  size = header_size;
  size += footer_size;
  partials = GNUNET_malloc (sizeof (char *) * ndownloads);
  partial_sizes = GNUNET_malloc (sizeof (unsigned int) * ndownloads);
  EXITIF (0 == ndownloads);
  for (cnt = 0, dwn = dwn_head; cnt < ndownloads; cnt++, dwn=dwn->next)
  {
    EXITIF (NULL == dwn);
    psize = GNUNET_asprintf (&partials[cnt],
                             "<li><a href=\"/contract?product=%u\">%s</a></li>",
                             cnt,
                             GNUNET_STRINGS_get_short_name (dwn->filename));
    EXITIF (psize < 0);
    size += psize;
    partial_sizes [cnt] = psize;
  }
  page = GNUNET_malloc (size);
  size = 0;
  (void) memcpy (page, header, header_size);
  size += header_size;
  for (cnt = 0; cnt < ndownloads; cnt++)
  {
    (void) memcpy (page + size, partials[cnt], partial_sizes[cnt]);
    size += partial_sizes[cnt];
  }
  (void) memcpy (page + size, footer, footer_size);
  size += footer_size;
  list_products_resp = MHD_create_response_from_buffer (size, page, MHD_RESPMEM_MUST_FREE);
  ret = GNUNET_OK;

 EXITIF_exit:
  for (cnt = 0; cnt < ndownloads; cnt++)
    GNUNET_free_non_null (partials[cnt]);
  GNUNET_free_non_null (partials);
  GNUNET_free_non_null (partial_sizes);
  return ret;
}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  char *keyfile;
  struct MERCHANT_MintInfo *mint_infos;
  unsigned int nmints;
  unsigned int cnt;

  result = GNUNET_SYSERR;
  keyfile = NULL;
  mint_infos = NULL;
  shutdown_task = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                                &do_shutdown, NULL);
  if (NULL == data_dir)
  {
    LOG_ERROR ("Data directory for download files is missing.  It can be given with the `-d' option\n");
    goto EXITIF_exit;
  }
  EXITIF (GNUNET_SYSERR == (nmints = TALER_MERCHANT_parse_mints (config,
                                                                 &mint_infos)));
  EXITIF (NULL == (wire = TALER_MERCHANT_parse_wireformat_sepa (config)));
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_filename (config,
                                                                "merchant",
                                                                "KEYFILE",
                                                                &keyfile));
  EXITIF (NULL == (privkey = GNUNET_CRYPTO_eddsa_key_create_from_file (keyfile)));
  EXITIF (0 == GNUNET_DISK_directory_scan (data_dir,
                                           &add_download_file,
                                           NULL));
  EXITIF (GNUNET_SYSERR == build_list_product_response ());
  EXITIF (NULL == (db_conn = MERCHANT_DB_connect (config)));
  EXITIF (GNUNET_OK != MERCHANT_DB_initialise (db_conn, dry));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_number (config,
                                                 "merchant",
                                                 "port",
                                                 &port));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "hostname",
                                                 &hostname));
  EXITIF (NULL == (mctx = TALER_MINT_init ()));
  EXITIF (NULL == (mints_map = GNUNET_CONTAINER_multipeermap_create (nmints, GNUNET_YES)));
  for (cnt = 0; cnt < nmints; cnt++)
  {
    struct Mint *mint;

    mint = GNUNET_new (struct Mint);
    mint->pubkey = mint_infos[cnt].pubkey;
    mint->conn = TALER_MINT_connect (mctx,
                                    mint_infos[cnt].hostname,
                                    mint_infos[cnt].port,
                                    &mint->pubkey);
    EXITIF (NULL == mint->conn);
    EXITIF (GNUNET_SYSERR == GNUNET_CONTAINER_multipeermap_put
            (mints_map,
             (struct GNUNET_PeerIdentity *) &mint->pubkey,
             mint,
             GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_FAST));
  }
  MHD_set_panic_func (&mhd_panic_cb, NULL);
  mhd = MHD_start_daemon (MHD_USE_DEBUG, //| MHD_USE_TCP_FASTOPEN,
                          (unsigned short) port,
                          NULL, NULL,
                          &url_handler, NULL,
                          //MHD_OPTION_TCP_FASTOPEN_QUEUE_SIZE,
                          //(unsigned int) 16,
                          MHD_OPTION_END);
  EXITIF (NULL == mhd);
  EXITIF (GNUNET_SYSERR == poll_mhd ());
  GNUNET_CRYPTO_hash (wire, sizeof (*wire), &h_wire);
  result = GNUNET_OK;

 EXITIF_exit:
  if (NULL != mint_infos)
  {
    for (cnt = 0; cnt < nmints; cnt++)
      GNUNET_free (mint_infos[cnt].hostname);
    GNUNET_free (mint_infos);
  }
  GNUNET_free_non_null (keyfile);
  if (GNUNET_OK != result)
    GNUNET_SCHEDULER_shutdown ();
}


/**
 * The main function of the serve tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'t', "temp", NULL,
     gettext_noop ("Use temporary database tables"), GNUNET_NO,
     &GNUNET_GETOPT_set_one, &dry},
    {'d', "dir", "DIRECTORY",
     gettext_noop ("Directory of the data files to serve"), GNUNET_YES,
     &GNUNET_GETOPT_set_string, &data_dir},
    GNUNET_GETOPT_OPTION_END
  };

  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-merchant-serve",
                          "Serve merchant's HTTP interface",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
