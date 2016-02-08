<?php
/**
 * Return a contract proposition to forward to the backend
 * Note that `teatax` is an associative array representing a
 * Taler-style amount (so it has the usual <amount,fration,currency>
 * triple). Moreover, `teatax` should be a *list* of taxes
 */
function generate_contract($amount_value,
                           $amount_fraction,
			   $currency,
			   $transaction_id,
			   $desc,
			   $p_id,
			   $teatax,
			   $now,
			   $fulfillment_url){
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
  			    'fulfillment_url' => $fulfillment_url,
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
  $json = json_encode (array ('contract' => $contract, JSON_PRETTY_PRINT));
  return $json;
}



/**
 * Feed `$json` to the backend and return the "(pecl) http response object"
 * corresponding to the `$backend_relative_url` call
 */
function give_to_backend($backend_host, $backend_relative_url, $json){
  $url = (new http\URL("http://$backend_host"))
    ->mod(array ("path" => $backend_relative_url), http\Url::JOIN_PATH);
  
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
