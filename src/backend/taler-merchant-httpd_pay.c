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
 * @file merchant/backend/taler-merchant-httpd.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_signatures.h>
#include <taler/taler_amount_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"

extern struct MERCHANT_Mint *mints;
extern const struct MERCHANT_WIREFORMAT_Sepa *wire;
extern PGconn *db_conn;
extern long long salt;
extern unsigned int nmints;
extern struct GNUNET_TIME_Relative edate_delay;

/**
 * Fetch the deposit fee related to the given coin aggregate.
 * @param connection the connection to send an error response to
 * @param coin_aggregate a coin "aggregate" is the JSON set of
 * values contained in a single cell of the 'coins' array sent
 * in a payment
 * @param deposit_fee where to store the resulting deposi fee
 * @param mint_index the index which points the chosen mint within
 * the global 'mints' array
 * @return GNUNET_OK if successful, GNUNET_NO if the data supplied
 * is invalid, GNUNET_SYSERR upon internal errors
 */
int
deposit_fee_from_coin_aggregate (struct MHD_Connection *connection,
                                 json_t *coin_aggregate,
                                 struct TALER_Amount *deposit_fee,
			         unsigned int mint_index)
{
  int res;
  const struct TALER_MINT_Keys *keys;
  const struct TALER_MINT_DenomPublicKey *denom_details;
  struct TALER_DenominationPublicKey denom;

  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_denomination_public_key ("denom_pub", &denom)
  };
  
  res = TMH_PARSE_json_data (connection,
                             coin_aggregate,
			     spec);
  if (GNUNET_OK != res)
    return res; /* may return GNUNET_NO */

  if (1 == mints[mint_index].pending) 
    return GNUNET_SYSERR;
  keys = TALER_MINT_get_keys (mints[mint_index].conn);

  denom_details = TALER_MINT_get_denomination_key (keys, &denom);
  *deposit_fee = denom_details->fee_deposit;
  return GNUNET_OK;
}

/**
 * Accomplish this payment.
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure
 * (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a
 * upload_data
 * @return MHD result code
 */
int
MH_handler_pay (struct TMH_RequestHandler *rh,
                struct MHD_Connection *connection,
                void **connection_cls,
                const char *upload_data,
                size_t *upload_data_size)
{

  json_t *root;
  json_t *coins;
  char *chosen_mint;
  size_t chosen_mint_length;
  json_t *coin_aggregate;
  unsigned int coins_cnt;
  unsigned int mint_index; //pointing global array
  uint64_t transaction_id;
  int res;

  struct TALER_Amount max_fee;
  struct TALER_Amount acc_fee;
  struct TALER_Amount coin_fee;
  struct GNUNET_TIME_Absolute edate;
  struct GNUNET_TIME_Absolute timestamp;

  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_array ("coins", &coins),
    TMH_PARSE_member_variable ("mint",
                              (void *) &chosen_mint,
			      &chosen_mint_length), 

    TMH_PARSE_member_amount ("max_fee", &max_fee),
    TMH_PARSE_member_time_abs ("timestamp", &timestamp),
    TMH_PARSE_member_uint64 ("transaction_id", &transaction_id),
  };
  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES;

  //printf ("/pay\n");

  res = TMH_PARSE_json_data (connection,
                             root,
			     spec);

  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;

  /* 1 Check if the chosen mint is among the merchant's preferred.

    An error in this case could be due to:

    * the wallet indicated a non existent mint
    * the wallet indicated a non trusted mint

    NOTE: by preventively checking this, the merchant
    avoids getting HTTP response codes from random
    websites that may mislead the wallet in the way
    of managing the error. Of course, that protect the
    merchant from POSTing coins to untrusted mints.

   */
  for (mint_index = 0; mint_index < nmints; mint_index++)
  {
    if (0 == strncmp (mints[mint_index].hostname,
                      chosen_mint, chosen_mint_length))
      break;
    mint_index = -1;
  }

  if (-1 == mint_index);
    return TMH_RESPONSE_reply_external_error (connection, "unknown mint"); 

  printf ("mint idx %d", mint_index);

  /* no 'edate' from frontend. Generate it here; it will be timestamp
    + a edate delay supplied in config file */
  if (NULL == json_object_get (root, "edate"))
  {
    edate = GNUNET_TIME_absolute_add (timestamp, edate_delay);
    if (-1 == json_object_set (root, "edate", TALER_json_from_abs (edate)))
      return MHD_NO;
  }

  coins_cnt = json_array_size (coins);
  
  if (0 == coins_cnt)
    return TMH_RESPONSE_reply_external_error (connection, "no coins given"); 

  json_array_foreach (coins, coins_cnt, coin_aggregate)
  {
    res = deposit_fee_from_coin_aggregate (connection,
                                           coin_aggregate,
		                           &coin_fee,
				           mint_index);
    if (GNUNET_OK != res)
      return MHD_NO; 

    if (0 == coins_cnt)
      acc_fee = coin_fee;
    else
      TALER_amount_add (&acc_fee,
                        &acc_fee,
			&coin_fee);
  }

  if (-1 == TALER_amount_cmp (&max_fee, &acc_fee))
    return MHD_HTTP_NOT_ACCEPTABLE;
  
  /* 3 For each coin in DB

       a. Generate a deposit permission
       b. store it and its tid in DB
       c. POST to the mint (see mint-lib for this)
          (retry until getting a persisten state)
  */
  /* 4 Return response code: success, or whatever data the
    mint sent back regarding some bad coin */
}
