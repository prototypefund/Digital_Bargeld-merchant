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

  To test this feature from the command line, issue:

  - $ curl http://merchant_url/generate_taler_contract.php?cli_debug=yes
    if the whole "journey" to the backend is begin tested
  - $ curl http://merchant_url/generate_taler_contract.php?backend_test=no
    if just the frontend job is being tested
 */


register_shutdown_function(function() {
  $lastError = error_get_last();

  if (!empty($lastError) && $lastError['type'] == E_ERROR) {
    header('Status: 500 Internal Server Error');
    header('HTTP/1.0 500 Internal Server Error');
  }
});


$cli_debug = false;
$backend_test = true;

if (isset($_GET['cli_debug']) && $_GET['cli_debug'] == 'yes')
  $cli_debug = true;

if (isset($_GET['backend_test']) && $_GET['backend_test'] == 'no')
{
  $cli_debug = true;
  $backend_test = false;
}

// 1) recover the session information
session_start();
if (!$cli_debug && (! isset($_SESSION['receiver'])))
{
  http_response_code (404);
  echo "Please select a contract before getting to this page...";
  exit (0);
}

/* Obtain session state */
if (!$cli_debug)
{
  $receiver = $_SESSION['receiver'];
  $amount_value = intval ($_SESSION['amount_value']);
  $amount_fraction = intval ($_SESSION['amount_fraction']);
  $currency = $_SESSION['currency'];
}
else
{
  $receiver = "Test Receiver";
  $amount_value = 5;
  $amount_fraction = 5;
  $currency = "KUDOS";

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
// The tax for this deal
$teatax = array ('value' => 1,
                 'fraction' => 0,
		 'currency' => $currency);

// Take a timestamp
$now = new DateTime('now');

// pack the JSON for the contract 
// --- FIXME: exact format needs review!
$json = json_encode (array ('amount' => array ('value' => $amount_value,
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
			    'pay_url' => "/taler/pay",
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
								     'ZIP code' => 4908))), JSON_PRETTY_PRINT);
if ($cli_debug && !$backend_test)
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
  echo "Error while generating the contract";
}
else
{
  // send the contract back to the wallet without touching it
  echo $resp->body->toString ();
}
?>
