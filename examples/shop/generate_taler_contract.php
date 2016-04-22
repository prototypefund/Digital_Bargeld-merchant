<?php 
/*
  This file is part of GNU TALER.
  Copyright (C) 2014-2016 INRIA

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
  . '?timestamp=' . $now->getTimestamp()
  . '&receiver=' . urlencode($receiver)
  . '&aval=' . urlencode($amount_value)
  . '&afrac=' . urlencode($amount_fraction)
  . '&acurr=' . urlencode($currency)
  . '&tid=' . $transaction_id;

$contract = generate_contract(array(
  "amount_value" => $amount_value,
  "amount_fraction" => $amount_fraction,
  "currency" => $currency,
  "refund_delta" => 'P3M',
  "transaction_id" => $transaction_id,
  "description" => $desc,
  "product_id" => "unused",
  "correlation_id" => "",
  "merchant_name" => "Kudos Inc.",
  "taxes" => array(),
  "now" => $now,
  "fulfillment_url" => $fulfillment_url)
);

file_put_contents("/tmp/ff.link", $fulfillment_url . "\r\n");

$resp = give_to_backend("backend/contract", $contract);

// Our response code is the same we got from the backend:
http_response_code($resp->getResponseCode());

// Now generate our body  
if ($resp->getResponseCode() != 200)
{
  echo json_encode(array(
    'error' => "internal error",
    'hint' => "backend indicated error",
    'detail' => $resp->body->toString()
  ), JSON_PRETTY_PRINT);
}
else
{
  # no state here
  $got_json = json_decode($resp->body->toString(), true);
  echo json_encode ($got_json, JSON_PRETTY_PRINT);
}
?>
