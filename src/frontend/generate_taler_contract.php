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


include 'util.php';

session_start();

if (!isset($_SESSION['receiver']))
{
  http_response_code (400);
  die();
}

$receiver = $_SESSION['receiver'];
$receiver = $_SESSION['receiver'];
$amount_value = intval ($_SESSION['amount_value']);
$amount_fraction = intval ($_SESSION['amount_fraction']);
$currency = $_SESSION['currency'];

/* Fill in variables for simple JSON contract */
// fake product id 
// --- FIXME: base on receiver for more realism!
$p_id = rand(0,1001);
// generate a front-end transaction id. 
// In production context, we might want to
// record this value somewhere together
// with the rest of the contract data.
$transaction_id = rand(0, 1001);
// Human-readable description of this deal
$desc = "Donation to " . $receiver;
// The tax for this deal
$teatax = array ('value' => 1,
                 'fraction' => 0,
		 'currency' => $currency);

// Take a timestamp
$now = new DateTime('now');

// pack the JSON for the contract 
// --- FIXME: exact format needs review!
$contract = array ('amount' => array ('value' => $amount_value,
			                       'fraction' => $amount_fraction,
                                               'currency' => $currency),
			    'max_fee' => array ('value' => 3,
			                        'fraction' => 01010,
						'currency' => $currency),
                            'transaction_id' => $transaction_id,
                            'products' => array (
			       array ('description' => $desc,
			              'quantity' => 1,
			              'price' => array ('value' => $amount_value,
			                                'fraction' => $amount_fraction,
                                                        'currency' => $currency),
				      'product_id' => $p_id,
				      'taxes' => array (array ('teatax' => $teatax)),
			              'delivery_date' => "Some Date Format",
			              'delivery_location' => 'LNAME1')),
			    'timestamp' => "/Date(" . $now->getTimestamp() . ")/",
			    'expiry' => "/Date(" . $now->add(new DateInterval('P2W'))->getTimestamp() . ")/",
			    'refund_deadline' => "/Date(" . $now->add(new DateInterval('P3M'))->getTimestamp() . ")/",
			    'merchant' => array ('address' => 'LNAME2',
			                         'name' => 'test merchant',
					         'jurisdiction' => 'LNAME3'),

                            'locations' => array ('LNAME1' => array ('country' => 'Test Country',
						                     'city' => 'Test City',
						                     'state' => 'Test State',
							             'region' => 'Test Region',
								     'province' => 'Test Province',
								     'ZIP code' => 4908,
								     'street' => 'test street',
								     'street number' => 20),
						  'LNAME2' => array ('country' => 'Test Country',
						                     'city' => 'Test City',
						                     'state' => 'Test State',
							             'region' => 'Test Region',
								     'province' => 'Test Province',
								     'ZIP code' => 4908,
								     'street' => 'test street',
								     'street number' => 20),
						  'LNAME3' => array ('country' => 'Test Country',
						                     'city' => 'Test City',
						                     'state' => 'Test State',
							             'region' => 'Test Region',
								     'province' => 'Test Province',
                                                                     'ZIP code' => 4908)));

$json = json_encode(array('contract' => $contract, JSON_PRETTY_PRINT));

$url = url_join("http://".$_SERVER["HTTP_HOST"], "backend/contract");

$req = new http\Client\Request("POST",
                               $url,
                               array ("Content-Type" => "application/json"));

$req->getBody()->append ($json);

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
  echo "Error while generating the contract";
  echo $resp->body->toString ();
}
else
{
  $got_json = json_decode ($resp->body->toString (), true);
  $got_json['pay_url'] = url_rel("pay.php");
  $got_json['exec_url'] = url_rel("execute.php") . "?H_contract=" . $got_json["H_contract"];
  $_SESSION['H_contract'] = $got_json["H_contract"];
  echo json_encode ($got_json, JSON_PRETTY_PRINT);
}
?>
