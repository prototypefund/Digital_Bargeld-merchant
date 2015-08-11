<?php 

/*

  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>

*/

/*  

  Here goes all the Taler paying logic. The steps are

  1. recover the session

  2. generate the JSON to forward to the backend

  3. route back to the wallet the certificate just gotten

*/


// recover the session
session_start();
if(!isset($_SESSION['maydonate'])){
  http_response_code(404);
  echo "Please try to donate before getting to this page :)";
}

else{
  // fake product id
  $p_id = rand(0,1001);
  // generate a transaction/certificate id. In production context, it's wishable to
  // record this value
  $trans_cert_id = rand(0, 1001);
  // fake a human readable description of this deal
  $desc = "donation aimed to stop the ants' massacre on hicking paths";
  // fake the price's integer (actually, the system is testishly suited for just 10 EUR coins)
  $value = 10;
  // fake the price's fractional
  $fraction = 0;
  // hardcode the currency
  $currency = "EUR";

  // pack the JSON
  $json  = json_encode (array ('desc' => $desc, 'product' => $p_id, 'cid' => $trans_cert_id,
                               'price' => array ('value' => $value,
			                         'fraction' => $fraction,
						 'currency' => $currency)));
  // test
  // echo $json;
  // echo phpinfo ();
  
  // crafting the request
  $req = new http\Client\Request ("POST",
                                  "http://" . $_SERVER["SERVER_NAME"] . "/backend" . "/contract",
                                  //"http://localhost:9898/",
				  array ("Content-Type" => "application/json"));
  $req->getBody()->append ($json);

  $client = new http\Client;
  $client->enqueue($req)->send ();
  $resp = $client->getResponse ();
  $status_code = $resp->getResponseCode ();
  http_response_code ($status_code);
  

  if ($status_code != 200){
    echo "Error while generating the certificate, response code : " . $status_code;
  }
  // send the contract back to the wallet without touching it
  else{
    echo $resp->body->toString ();
  }

}


?>
