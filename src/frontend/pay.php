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

$cli_debug = false;
$backend_test = true;

if ($_GET['cli_debug'] == 'yes')
  $cli_debug = true;

if ($_GET['backend_test'] == 'no')
{
  $cli_debug = true;
  $backend_test = false;
}

$post_body = file_get_contents('php://input');

$now = new DateTime('now');
$edate = array ('edate' =>
               "/Date(" . $now->add(new DateInterval('P2W'))->getTimestamp() . ")/");

$deposit_permission = json_decode ($post_body, true);
$max_fee = array ('max_fee' => array ('value' => 3,
		                      'fraction' => 8,
		                      'currency' => "KUDOS"));

$new_deposit_permission = array_merge ($deposit_permission, $max_fee);
$new_deposit_permission_edate = array_merge ($new_deposit_permission, $edate);

/* Craft the HTTP request, note that the backend
  could be on an entirely different machine if
  desired. */

if ($cli_debug && !$backend_test)
{

  /* DO NOTE the newline at the end of 'echo's argument */
  //echo json_encode ($new_deposit_permission_edate, JSON_PRETTY_PRINT)
  echo json_encode ($new_deposit_permission, JSON_PRETTY_PRINT)
  . "\n";
  exit;
}

$req = new http\Client\Request ("POST",
                                "http://" . $_SERVER["SERVER_NAME"] . "/backend/pay",
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
  /* error: just forwarding to the wallet what
    gotten from the backend (which is forwarding 'as is'
    the error gotten from the mint) */
  echo $resp->body->toString ();
  
}
else
{
  header("Content-Type", "text/html");
  echo '<html><title>GNU Taler payment</title><body>Payment succeeded!</body></html>\n';
}

?>
