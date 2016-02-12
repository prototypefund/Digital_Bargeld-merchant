<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * This file should:
 *
 * 1. check if some article is going to be bought
 * 2. check if the wallet is installed TODO
 * 3. get the contract (having the teaser as product detail) to the wallet
 *
 */
include("../frontend_lib/merchants.php");
include("../frontend_lib/util.php");
include("./blog_lib.php");
session_start();
$article = get($_GET['article']);
if (null == $article){
  echo "Please land here just to buy articles";
  die();
  }

// send contract
$transaction_id = rand(0, 1001);
$p_id = hexdec(substr(sha1($article), -5));
$teatax = array ('value' => 1,
                 'fraction' => 0,
		 'currency' => "KUDOS");
$now = new DateTime('now');
$teaser = get_teaser($article);

$amount_value = 1;
$amount_fraction = 0;
$currency = "KUDOS";
$transaction_id = rand(0, 1001);
// Include all information so we can
// restore the contract without storing it
$fulfillment_url = url_rel("essay_fulfillment.php")
  . '&uuid=${H_contract}'; //<= super weird: that should be a '?', not '&', but works
//file_put_contents("/tmp/debg1", $fulfillment_url);
$contract_json = generate_contract($amount_value,
                                   $amount_fraction,
				   $currency,
				   $transaction_id,
				   trim($teaser->nodeValue),
				   $p_id,
				   $teatax,
				   $now,
				   $fulfillment_url);
$resp = give_to_backend($_SERVER["HTTP_HOST"],
                        "backend/contract",
	                $contract_json);

// Our response code is the same we got from the backend:
$status_code = $resp->getResponseCode();
http_response_code ($status_code);

// Now generate our body  
if ($status_code != 200)
{
  echo json_encode(array(
    'error' => "internal error",
    'hint' => "backend indicated error",
    'detail' => $resp->body->toString()
  ), JSON_PRETTY_PRINT);
}
else
{
  $got_json = json_decode($resp->body->toString(), true);
  $hc = $got_json["H_contract"];

  $payments = &pull($_SESSION, "payments", array());
  $payments[$hc] = array(
    'article' => $article,
  );

  echo json_encode ($got_json, JSON_PRETTY_PRINT);
}
?>
