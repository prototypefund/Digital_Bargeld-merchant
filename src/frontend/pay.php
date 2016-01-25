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

/*
  This serving module adds the 'max_fee' field to the object which
  sends to the backend, and optionally the field 'edate' (indicating
  to the mint the tollerated deadline to receive funds for this payment)
  NOTE: 'max_fee' must be consistent with the same value indicated within
  the contract; thus, a "real" merchant must implement such a mapping

 */

include 'util.php';

function respond_success() {
  $_SESSION['payment_ok'] = true;
  $json = json_encode(
    array(
      "fulfillment_url" => url_rel("fulfillment.php")));
  echo $json;
}

session_start();

if (!isset($_SESSION['H_contract']))
{
  $json = json_encode(
    array("error" => "No session active"));
  echo $json;
  http_response_code (401);
  die();
}

if (isset($_SESSION['payment_ok']) && $_SESSION['payment_ok'] == true)
{
  respond_success();
  die();
}

$post_body = file_get_contents('php://input');

$now = new DateTime('now');
$edate = array ('edate' =>
               "/Date(" . $now->add(new DateInterval('P2W'))->getTimestamp() . ")/");

$deposit_permission = json_decode ($post_body, true);

$to_add = array('max_fee' => array('value' => 3,
                                   'fraction' => 8,
                                   'currency' => $_SESSION['currency']),
                'amount' => array('value' => $_SESSION['amount_value'],
                                  'fraction' => $_SESSION['amount_fraction'],
		                  'currency' => $_SESSION['currency']));

$new_deposit_permission = array_merge($deposit_permission, $to_add);
$new_deposit_permission_edate = array_merge($new_deposit_permission, $edate);

/* Craft the HTTP request, note that the backend
  could be on an entirely different machine if
  desired. */

// Backend is relative to the shop site.
/**
 * WARNING: the "shop site" is '"http://".$_SERVER["HTTP_HOST"]'
 * So do not attach $_SERVER["REQUEST_URI"] before proxying requests
 * to the backend
 */
$url = url_join("http://".$_SERVER["HTTP_HOST"], "backend/pay");

$req = new http\Client\Request("POST",
                               $url,
                               array ("Content-Type" => "application/json"));
$req->getBody()->append (json_encode ($new_deposit_permission));

// Execute the HTTP request
$client = new http\Client;
$client->enqueue($req)->send ();

// Fetch the response
$resp = $client->getResponse ();
$status_code = $resp->getResponseCode ();

// Our response code is the same we got from the backend:
http_response_code ($status_code);

// Now generate our body  
if ($status_code != 200)
{
  $json = json_encode(
    array(
      "error" => "backend error",
      "status" => $status_code,
      "detail" => $resp->body->toString ()));
  echo $json;
}
else
{
  respond_success();
  die();
}

?>
