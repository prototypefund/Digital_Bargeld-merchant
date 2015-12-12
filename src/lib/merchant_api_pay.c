/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_pay.c
 * @brief Implementation of the /pay request of the merchant's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include "taler_merchant_service.h"
#include "merchant_api_json.h"
#include "merchant_api_context.h"
#include "taler_signatures.h"


/**
 * @brief A Pay Handle
 */
struct TALER_MERCHANT_Pay
{

  /**
   * The url for this request.
   */
  char *url;

  /**
   * JSON encoding of the request to POST.
   */
  char *json_enc;

  /**
   * Handle for the request.
   */
  struct MAC_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_PayCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Download buffer
   */
  struct MAC_DownloadBuffer db;

};



/**
 * Function called when we're done processing the
 * HTTP /pay request.
 *
 * @param cls the `struct TALER_MERCHANT_PayHandle`
 * @param eh the curl request handle
 */
static void
handle_pay_finished (void *cls,
                     CURL *eh)
{
  struct TALER_MERCHANT_PayHandle *ph = cls;
  long response_code;
  json_t *json;

  ph->job = NULL;
  json = MAC_download_get_result (&ph->db,
                                  eh,
                                  &response_code);
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the merchant is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_FORBIDDEN:
    break;
  case MHD_HTTP_UNAUTHORIZED:
    /* Nothing really to verify, merchant says one of the signatures is
       invalid; as we checked them, this should never happen, we
       should pass the JSON reply to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  ph->cb (ph->cb_cls,
          response_code,
          json);
  json_decref (json);
  TALER_MERCHANT_pay_cancel (ph);
}


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 *
 * @param merchant the merchant context
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_wallet (struct TALER_MERCHANT_Context *merchant,
                           const struct GNUNET_HashCode *h_wire,
                           const struct GNUNET_HashCode *h_contract,
                           struct GNUNET_TIME_Absolute timestamp,
                           uint64_t transaction_id,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           struct GNUNET_TIME_Absolute refund_deadline,
                           unsigned int num_coins,
                           const struct TLAER_MERCHANT_PayCoin *coins,
                           TALER_MERCHANT_PayCallback pay_cb,
                           void *pay_cb_cls)
{
  struct TALER_MERCHANT_PayHandle *ph;
  json_t *pay_obj;
  CURL *eh;
  struct GNUNET_HashCode h_wire;
  struct TALER_Amount amount_without_fee;

  pay_obj = json_pack ("{s:o, s:O," /* f/wire */
                       " s:o, s:o," /* H_wire, H_contract */
                       " s:o, s:o," /* coin_pub, denom_pub */
                       " s:o, s:o," /* ub_sig, timestamp */
                       " s:I, s:o," /* transaction id, merchant_pub */
                       " s:o, s:o," /* refund_deadline, wire_deadline */
                       " s:o}",     /* coin_sig */
                       "f", TALER_json_from_amount (amount),
                       "wire", wire_details,
                       "H_wire", TALER_json_from_data (&h_wire,
                                                       sizeof (h_wire)),
                       "H_contract", TALER_json_from_data (h_contract,
                                                           sizeof (struct GNUNET_HashCode)),
                       "coin_pub", TALER_json_from_data (coin_pub,
                                                         sizeof (*coin_pub)),
                       "denom_pub", TALER_json_from_rsa_public_key (denom_pub->rsa_public_key),
                       "ub_sig", TALER_json_from_rsa_signature (denom_sig->rsa_signature),
                       "timestamp", TALER_json_from_abs (timestamp),
                       "transaction_id", (json_int_t) transaction_id,
                       "merchant_pub", TALER_json_from_data (merchant_pub,
                                                             sizeof (*merchant_pub)),
                       "refund_deadline", TALER_json_from_abs (refund_deadline),
                       "edate", TALER_json_from_abs (wire_deadline),
                       "coin_sig", TALER_json_from_data (coin_sig,
                                                         sizeof (*coin_sig))
                       );

  ph = GNUNET_new (struct TALER_MERCHANT_PayHandle);
#if 0
  ph->merchant = merchant;
  ph->cb = cb;
  ph->cb_cls = cb_cls;
  ph->url = MAH_path_to_url (merchant, "/pay");
  ph->depconf.purpose.size = htonl (sizeof (struct TALER_PayConfirmationPS));
  ph->depconf.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONFIRM_PAY);
  ph->depconf.h_contract = *h_contract;
  ph->depconf.h_wire = h_wire;
  ph->depconf.transaction_id = GNUNET_htonll (transaction_id);
  ph->depconf.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  ph->depconf.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  TALER_amount_subtract (&amount_without_fee,
                         amount,
                         &dki->fee_pay);
  TALER_amount_hton (&ph->depconf.amount_without_fee,
                     &amount_without_fee);
  ph->depconf.coin_pub = *coin_pub;
  ph->depconf.merchant = *merchant_pub;
  ph->amount_with_fee = *amount;
  ph->coin_value = dki->value;

  eh = curl_easy_init ();
  GNUNET_assert (NULL != (ph->json_enc =
                          json_dumps (pay_obj,
                                      JSON_COMPACT)));
  json_decref (pay_obj);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   ph->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   ph->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (ph->json_enc)));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEFUNCTION,
                                   &MAC_download_cb));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_WRITEDATA,
                                   &ph->db));
  ctx = MAH_handle_to_context (merchant);
  ph->job = MAC_job_add (ctx,
                         eh,
                         GNUNET_YES,
                         &handle_pay_finished,
                         ph);
  return ph;
#endif
  return NULL;
}



/**
 * Pay a merchant.  API for frontends talking to backends. Here,
 * the frontend does not have the coin's private keys, but just
 * the public keys and signatures.  Note the sublte difference
 * in the type of @a coins compared to #TALER_MERCHANT_pay().
 *
 * @param merchant the merchant context
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_frontend (struct TALER_MERCHANT_Context *merchant,
                             const struct GNUNET_HashCode *h_wire,
                             const struct GNUNET_HashCode *h_contract,
                             struct GNUNET_TIME_Absolute timestamp,
                             uint64_t transaction_id,
                             const struct TALER_MerchantPublicKeyP *merchant_pub,
                             struct GNUNET_TIME_Absolute refund_deadline,
                             unsigned int num_coins,
                             const struct TALER_MERCHANT_PaidCoin *coins,
                             TALER_MERCHANT_PayCallback pay_cb,
                             void *pay_cb_cls)
{
  GNUNET_break (0); // FIXME: not implemented!
  return NULL;
}


/**
 * Cancel a pay permission request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param pay the pay permission request handle
 */
void
TALER_MERCHANT_pay_cancel (struct TALER_MERCHANT_PayHandle *pay)
{
  if (NULL != pay->job)
  {
    MAC_job_cancel (pay->job);
    pay->job = NULL;
  }
  GNUNET_free_non_null (pay->db.buf);
  GNUNET_free (pay->url);
  GNUNET_free (pay->json_enc);
  GNUNET_free (pay);
}


/* end of merchant_api_pay.c */
