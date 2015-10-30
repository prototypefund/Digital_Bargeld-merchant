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
deposit_fee_from_coin_json (struct MHD_Connection *connection,
                            json_t *coin_aggregate,
                            struct TALER_Amount *deposit_fee,
			    unsigned int mint_index)
{
  int res;
  struct TALER_DenominationPublicKey denom;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_denomination_public_key ("denom_pub", &denom); 
  };
  
  res = TMH_PARSE_json_data (connection,
                             coin_aggregate,
			     spec);
  if (GNUNET_OK != res)
    return res;
  /* Iterate over the mint keys to get the wanted data */
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
  json_t *chosen_mint;
  json_t *coin_aggregate;
  unsigned int ncoins;
  unsigned int mint_index; //pointing global array
  int res;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_array ("coins", &coins); 
    TMH_PARSE_member_object ("mint", &chosen_mint); 
  };
  struct TALER_Amount max_deposit_fee;
  //struct TALER_Amount acc_deposit_fee;
  //struct TALER_Amount coin_deposit_fee;
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
                             coin_aggregate,
			     spec);

  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;

  /* 0 What if the coin gives zero-length coins array? */
  ncoins = json_array_size (coins);
  if (0 == ncoins)
    return TMH_RESPONSE_reply_external_error (connection,
                                              "empty coin array");
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
    if (0 == strcmp (mints[mint_index].hostname, json_string_value (chosen_mint))) 
      break;
    mint_index = -1;
  }

  if (-1 == mint_index)

  /* TODO notify the wallet that it indicated an unknown mint */

  /* 2 Check if the total deposit fee is \leq the limit */
  if (NULL == (coin_aggregate = json_array_get (coins, 0)))
    return MHD_NO;

  /* 3 For each coin in DB

       a. Generate a deposit permission
       b. store it in DB
       c. POST to the mint (see mint-lib for this)
          (retry until getting a persisten state)
  */
  /* 4 Return response code: success, or whatever data the
    mint sent back regarding some bad coin */
}
