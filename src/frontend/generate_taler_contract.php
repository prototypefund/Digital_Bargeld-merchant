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
  This code generates a Taler contract in JSON format. Key steps are:

  1. recover the PHP session with the contract information
  2. generate the JSON to forward to the backend
  3. forward the response with the contract from the backend to
     to the wallet
*/

$cli_debug = TRUE;

// 1) recover the session information
session_start();
if (!$cli_debug && ((! isset($_SESSION['receiver'])) ||
     (! isset($_SESSION['amount']))) )
{
  http_response_code (404);
  echo "Please select a contract before getting to this page...";
  echo "attempted : " . $_SESSION['receiver'];
  exit (0);
}

/* Obtain session state */
if (!$cli_debug)
{
  $receiver = $_SESSION['receiver'];
  $amount = intval ($_SESSION['amount']);
}
else
{
  $receiver = "Test Receiver";
  $amount = 5;

}

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
// Add the value
$value = $amount;
// We don't have a fraction.
$fraction = 0;
// This is our 'toy' currency
$currency = "EUR"; // NOTE: mint does NOT generate KUDOS denom. keys
// The tax for this deal
$teatax = array ('value' => 1,
                 'fraction' => 0,
		 'currency' => $currency);

// pack the JSON for the contract 
// --- FIXME: exact format needs review!
$json = json_encode (array ('amount' => array ('value' => $value,
			                       'fraction' => $fraction,
                                               'currency' => $currency),
			    'max fee' => array ('value' => 3,
			                        'fraction' => 0,
						'currency' => $currency),
                            'trans_id' => $transaction_id,
                            'details' =>
			      array ('items' =>
			              array ('description' => $desc,
					     'quantity' => 1,
			                     'itemprice' => array ('value' => $value,
			                                           'fraction' => $fraction,
                                                                   'currency' => $currency)),
					     'product_id' => $p_id,
					     'taxes' => array (array ('teatax' => $teatax)),
			             'delivery_date' => "Some Date Format",
			             'delivery location' => 'LNAME1',
			             'merchant' => array ('address' => 'LNAME2',
			                                  'name' => 'test merchant',
						          'jurisdiction' =>
						            array ('country' => 'Test Country',
						                   'city' => 'Test City',
						                   'state' => 'Test State',
							           'region' => 'Test Region',
								   'province' => 'Test Province',
								   'ZIP code' => 4908)),
			             'L-names' => array (array ('LNAME1' => 'Test Address 1',
			                                        'LNAME2' => 'Test Address 2')))));//,
                    //JSON_PRETTY_PRINT);

if ($cli_debug && FALSE)
{
  echo $json . "\n";
  exit;
}

// Craft the HTTP request, note that the backend
// could be on an entirely different machine if
// desired.
$req = new http\Client\Request ("POST",
                                "http://" . $_SERVER["SERVER_NAME"] . "/backend/contract",
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
  echo "Error while generating the contract, response code: " . $status_code . "\n";
}
else
{
  // send the contract back to the wallet without touching it
  echo $resp->body->toString ();
}
?>
