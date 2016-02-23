<?php 
/*
  This file is part of GNU TALER.
  Copyright (C) 2014-2016 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
 */

include '../../copylib/util.php';
include "../../copylib/config.php";
include "../../copylib/merchants.php";

session_start();

if (!isset($_SESSION['receiver']))
{
  http_response_code(400);
  die();
}

$receiver = $_SESSION['receiver'];
$amount_value = intval($_SESSION['amount_value']);
$amount_fraction = intval($_SESSION['amount_fraction']);
$currency = $_SESSION['currency'];

// generate a front-end transaction id. 
// In production context, we might want to
// record this value somewhere together
// with the rest of the contract data.
$transaction_id = rand(0, 2<<40);

// Human-readable description of this deal
$desc = "Donation to " . $receiver;

// Take a timestamp
$now = new DateTime('now');

// Include all information so we can
// restore the contract without storing it
$fulfillment_url = url_rel("fulfillment.php")
  . '?uuid=${H_contract}'
  . '&receiver=' . urlencode($receiver)
  . '&aval=' . urlencode($amount_value)
  . '&afrac=' . urlencode($amount_fraction)
  . '&acurr=' . urlencode($currency)
  . '&tid=' . $transaction_id;


  $contract = _generate_contract(array("amount_value" => $amount_value,
                                       "amount_fraction" => $amount_fraction,
                                       "currency" => $currency,
				       "refund_delta" => 'P3M',
  		 	               "transaction_id" => $transaction_id,
  				       "description" => $desc,
  				       "product_id" => $p_id,
  				       "correlation_id" => "",
				       "merchant_name" => "Kudos Inc.",
  				       "taxes" => array(),
  				       "now" => $now,
  				       "fulfillment_url" => $fulfillment_url));


// pack the JSON for the contract 

/*
$contract = array(
  'fulfillment_url' => $fulfillment_url,
  'amount' => array(
    'value' => $amount_value,
    'fraction' => $amount_fraction,
    'currency' => $currency
  ),
  'max_fee' => array(
    'value' => 3,
    'fraction' => 01010,
    'currency' => $currency
  ),
  'transaction_id' => $transaction_id,
  'products' => array(
    array(
      'description' => $desc,
      'quantity' => 1,
      'price' => array (
        'value' => $amount_value,
        'fraction' => $amount_fraction,
        'currency' => $currency
      ),
      'product_id' => $p_id,
    )
  ),
  'timestamp' => "/Date(" . $now->getTimestamp() . ")/",
  'expiry' => "/Date(" . $now->add(new DateInterval('P2W'))->getTimestamp() . ")/",
  'refund_deadline' => "/Date(" . $now->add(new DateInterval('P3M'))->getTimestamp() . ")/",
  'merchant' => array(
    'name' => 'Kudos Inc.'
  )
);
*/

$json = json_encode(array(
  'contract' => $contract
), JSON_PRETTY_PRINT);

$url = url_join("http://".$_SERVER["HTTP_HOST"], "backend/contract");

$req = new http\Client\Request("POST",
                               $url,
                               array ("Content-Type" => "application/json"));

$req->getBody()->append($contract);

// Execute the HTTP request
$client = new http\Client;
$client->enqueue($req)->send();

// Fetch the response
$resp = $client->getResponse();
$status_code = $resp->getResponseCode();

// Our response code is the same we got from the backend:
http_response_code($status_code);

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
    'receiver' => $receiver,
  );

  echo json_encode ($got_json, JSON_PRETTY_PRINT);
}
?>
