<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 INRIA

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
 * Return a contract proposition to forward to the backend
 * Note that `teatax` is an associative array representing a
 * Taler-style amount (so it has the usual <amount,fration,currency>
 * triple). Moreover, `teatax` should be a *list* of taxes
 */
function generate_contract($args){
  $contract = array ('amount' =>
                array ('value' => $args['amount_value'],
  		       'fraction' => $args['amount_fraction'],
                       'currency' => $args['currency']),
  		'max_fee' =>
		  array ('value' => 3,
  		         'fraction' => 01010,
  		         'currency' => $args['currency']),
                'transaction_id' => $args['transaction_id'],
                'products' => array (
  		   array ('description' => $args['description'],
  		          'quantity' => 1,
  		          'price' =>
			    array ('value' => $args['amount_value'],
  			           'fraction' => $args['amount_fraction'],
                                   'currency' => $args['currency']),
  		          'product_id' => $args['product_id'],
  		          'taxes' => $args['taxes'],
  		          'delivery_date' => "Some Date Format",
  		          'delivery_location' => 'LNAME1')),
  	        'timestamp' => "/Date(" . $args['now']->getTimestamp() . ")/",
  		'expiry' =>
		  "/Date(" . $args['now']->add(new DateInterval('P2W'))->getTimestamp() . ")/",
  		'refund_deadline' =>
		  "/Date(" . $args['now']->add(new DateInterval($args['refund_delta']))->getTimestamp() . ")/",
		'repurchase_correlation_id' => $args['correlation_id'],
		'fulfillment_url' => $args['fulfillment_url'],
  		'merchant' =>
		  array ('address' => 'LNAME2',
  		         'name' => $args['merchant_name'],
  		         'jurisdiction' => 'LNAME3'),
                'locations' =>
		  array ('LNAME1' =>
		    array ('country' => 'Test Country',
  			   'city' => 'Test City',
  			   'state' => 'Test State',
  			   'region' => 'Test Region',
  			   'province' => 'Test Province',
  			   'ZIP code' => 4908,
  			   'street' => 'test street',
  			   'street number' => 20),
  			 'LNAME2' =>
		    array ('country' => 'Test Country',
  		           'city' => 'Test City',
  		           'state' => 'Test State',
  		           'region' => 'Test Region',
  		           'province' => 'Test Province',
  		           'ZIP code' => 4908,
  		           'street' => 'test street',
  		           'street number' => 20),
  		         'LNAME3' =>
		    array ('country' => 'Test Country',
  		           'city' => 'Test City',
  		           'state' => 'Test State',
  		           'region' => 'Test Region',
  		           'province' => 'Test Province',
  		           'ZIP code' => 4908)));
  $json = json_encode (array ('contract' => $contract), JSON_PRETTY_PRINT);
  return $json;
}


/**
 * Feed `$json` to the backend and return the "(pecl) http response object"
 * corresponding to the `$backend_relative_url` call
 */
function give_to_backend($backend_relative_url, $json){
  $url = url_join("http://".$_SERVER["HTTP_HOST"], $backend_relative_url);
  
  $req = new http\Client\Request("POST",
                                 $url,
                                 array ("Content-Type" => "application/json"));
  
  $req->getBody()->append($json);
  
  // Execute the HTTP request
  $client = new http\Client;
  $client->enqueue($req)->send();
  return $client->getResponse();
}
?>
