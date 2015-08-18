<!DOCTYPE html>
<html>
<head>
<title>Fake Wire Transfer</title>
    <script>
        /*
        @licstart  The following is the entire license notice for the
        JavaScript code in this page.

        Copyright (C) 2014,2015 GNUnet e.V.

        The JavaScript code in this page is free software: you can
        redistribute it and/or modify it under the terms of the GNU
        General Public License (GNU GPL) as published by the Free Software
        Foundation, either version 3 of the License, or (at your option)
        any later version.  The code is distributed WITHOUT ANY WARRANTY;
        without even the implied warranty of MERCHANTABILITY or FITNESS
        FOR A PARTICULAR PURPOSE.  See the GNU GPL for more details.

        As additional permission under GNU GPL version 3 section 7, you
        may distribute non-source (e.g., minimized or compacted) forms of
        that code without the copy of the GNU GPL normally required by
        section 4, provided you include this license notice and a URL
        through which recipients can access the Corresponding Source.

        @licend  The above is the entire license notice
        for the JavaScript code in this page.
        */
    </script>
</head>
<body>
<!-- 
  This page's main aim is to forward the fake wire transfer
  request to the demonstrator and to inform the customer
  about the result.  In a real-world deployment, this
  page would not be required as the customer would do a 
  wire transfer with his bank instead.
  -->
<?php

// Evaluate form
$reserve_pk = $_POST['reserve_pk'];
$kudos_amount = $_POST['kudos_amount'];

// pack the JSON
$json = json_encode (array ('reserve_pub' => $reserve_pk, 
                            'execution_date' => "/Date(" . time() . ")/",
                            'wire' => array ('type' => 'test'),
                            'amount' => array ('value' => intval($kudos_amount),
	                                       'fraction' => 0,
					       'currency' => 'KUDOS')));

// craft the HTTP request
$req = new http\Client\Request ("POST",
                                "http://demo.taler.net/admin/add/incoming",
			        array ("Content-Type" => "application/json"));
$req->getBody()->append ($json);

// execute HTTP request
$client = new http\Client;
$client->enqueue($req)->send ();
$resp = $client->getResponse ();

// evaluate response
$status_code = $resp->getResponseCode ();
http_response_code ($status_code);

if ($status_code != 200) 
{
  echo "Error $status_code when faking the wire transfer. Please report to taler@gnu.org";
}
else
{
  echo "Pretend wire transfer successful. Go <a href=\"/\">back</a> and enjoy shopping!";
}
?>
</body>
</html>
