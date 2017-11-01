/*
  This file is part of TALER
  (C) 2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_tip-pickup.c
 * @brief implementation of /tip-pickup handler
 * @author Christian Grothoff
 */
#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_tip-pickup.h"


/**
 * Details about a planchet that the customer wants to obtain
 * a withdrawal authorization.  This is the information that
 * will need to be sent to the exchange to obtain the blind
 * signature required to turn a planchet into a coin.
 */
struct PlanchetDetail
{
  /**
   * Hash of the denomination public key.
   */
  struct GNUNET_HashCode denom_pub_hash;

  /**
   * Blinded coin (see GNUNET_CRYPTO_rsa_blind()).  Note: is malloc()'ed!
   */
  char *coin_ev;

  /**
   * Number of bytes in @a coin_ev.
   */
  size_t coin_ev_size;
};


/**
 * Prepare (and eventually execute) a pickup.  Computes
 * the "pickup ID" (by hashing the planchets and denomination keys),
 * resolves the denomination keys and calculates the total
 * amount to be picked up.  Then runs the pick up execution logic.
 *
 * @param connection MHD connection for sending the response
 * @param tip_id which tip are we picking up
 * @param planchets what planchets are to be signed blindly
 * @param planchets_len length of the @a planchets array
 * @return #MHD_YES if a response was generated, #MHD_NO if
 *         the connection ought to be dropped
 */
static int
prepare_pickup (struct MHD_Connection *connection,
		const struct GNUNET_HashCode *tip_id,
		const struct PlanchetDetail *planchets,
		unsigned int planchets_len)
{
#if 0
  char *exchange_uri;
  enum TALER_ErrorCode ec;
  struct GNUNET_HashCode pickup_id;
  struct TALER_ReservePrivateKeyP reserve_priv;

  ec = db->lookup_exchange_by_tip (db->cls,
				   tip_id,
				   &exchange_uri);
  // FIXME: error handling
  // FIXME: obtain exchange handle -- asynchronously!? => API bad!

  // Then resolve hashes to DK pubs and amounts
  for (unsigned int i=0;i<planchets_len;i++)
  {
  }
  // Total up the amounts & compute pickup_id

  ec = db->pickup_tip (db->cls,
		       &total,
		       tip_id,
		       &pickup_id,
		       &reserve_priv);
  // FIXME: error handling

  // build and sign withdraw orders!

  // build final response...
  
  if (TALER_EC_NONE != ec)
  {
    /* FIXME: be more specific in choice of HTTP status code */
    return TMH_RESPONSE_reply_internal_error (connection,
					      ec,
                                              "Database error approving tip");
  }
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s}",
                                       "status", "ok");
    
#endif
  
  return MHD_NO;
}


/**
 * Information we keep for individual calls
 * to requests that parse JSON, but keep no other state.
 */
struct TMH_JsonParseContext
{

  /**
   * This field MUST be first.
   * FIXME: Explain why!
   */
  struct TM_HandlerContext hc;

  /**
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
   */
  void *json_parse_context;
};


/**
 * Custom cleanup routine for a `struct TMH_JsonParseContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
json_parse_cleanup (struct TM_HandlerContext *hc)
{
  struct TMH_JsonParseContext *jpc = (struct TMH_JsonParseContext *) hc;

  TMH_PARSE_post_cleanup_callback (jpc->json_parse_context);
  GNUNET_free (jpc);
}


/**
 * Free the memory used by the planchets in the @a pd array
 * (but not the @a pd array itself).
 *
 * @param pd array of planchets
 * @param pd_len length of @a pd array
 */
static void
free_planchets (struct PlanchetDetail *pd,
		unsigned int pd_len)
{
  for (unsigned int i=0;i<pd_len;i++)
    GNUNET_free (pd[i].coin_ev);
}


/**
 * Parse the given @a planchet into the @a pd.
 *
 * @param connection connection to use for error reporting
 * @param planchet planchet data in JSON format
 * @param[out] pd where to store the binary data
 * @return #GNUNET_OK upon success, #GNUNET_NO if a response
 *    was generated, #GNUNET_SYSERR to drop the connection
 */
static int
parse_planchet (struct MHD_Connection *connection,
		const json_t *planchet,
		struct PlanchetDetail *pd)
{
  int ret;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("denom_pub_hash",
				 &pd->denom_pub_hash),
    GNUNET_JSON_spec_varsize ("coin_ev",
			      (void **) &pd->coin_ev,
			      &pd->coin_ev_size),
    GNUNET_JSON_spec_end()
  };
  
  ret = TMH_PARSE_json_data (connection,
			     planchet,
			     spec);
  return ret;
}


/**
 * Manages a /tip-pickup call, checking that the tip is authorized,
 * and if so, returning the withdrawal permissions.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_tip_pickup (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size)
{
  int res;
  struct GNUNET_HashCode tip_id;
  json_t *planchets;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("tip_id", &tip_id),
    GNUNET_JSON_spec_json ("planchets", &planchets),
    GNUNET_JSON_spec_end()
  };
  struct TMH_JsonParseContext *ctx;
  json_t *root;
  unsigned int num_planchets;

  if (NULL == *connection_cls)
  {
    ctx = GNUNET_new (struct TMH_JsonParseContext);
    ctx->hc.cc = &json_parse_cleanup;
    *connection_cls = ctx;
  }
  else
  {
    ctx = *connection_cls;
  }
  res = TMH_PARSE_post_json (connection,
                             &ctx->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES;

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_YES != res)
  {
    GNUNET_break_op (0);
    json_decref (root);
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  }
  num_planchets = json_array_size (planchets);
  if (num_planchets > 1024)
  {
    GNUNET_JSON_parse_free (spec);
    json_decref (root);
    /* FIXME: define proper ec for this! */
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_BAD_REQUEST,
                                         "{s:s}",
                                         "status", "too many planchets");
  }
  {
    struct PlanchetDetail pd[num_planchets];

    for (unsigned int i=0;i<num_planchets;i++)
    {
      if (GNUNET_OK !=
	  (res = parse_planchet (connection,
				 json_array_get (planchets,
						 i),
				 &pd[i])))
      {
	free_planchets (pd,
			i);
	GNUNET_JSON_parse_free (spec);
	json_decref (root);
	return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }
    }
    res = prepare_pickup (connection,
			  &tip_id,
			  pd,
			  num_planchets);
    free_planchets (pd,
		    num_planchets);
  }
  GNUNET_JSON_parse_free (spec);
  json_decref (root);
  return res;
}
