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

include '../../copylib/util.php';

$hc = get($_GET["uuid"]);
if (empty($hc))
{
  http_response_code(400);
  echo json_encode(array(
    "error" => "missing parameter",
    "parameter" => "uuid"
  ));
  return;
}

// TODO: check if contract body matches URL parameters,
// so we won't generate a response for the wrong receiver.
$receiver = get($_GET["receiver"]);
if (empty($receiver))
{
  http_response_code(400);
  echo json_encode(array(
    "error" => "missing parameter",
    "parameter" => "receiver"
  ));
  return;
}

$post_body = file_get_contents('php://input');
$deposit_permission = json_decode ($post_body, true);

/* Craft the HTTP request, note that the backend
  could be on an entirely different machine if
  desired. */

// Backend is relative to the shop site.
$url = url_rel("backend/pay");

$req = new http\Client\Request("POST",
                               $url,
                               array("Content-Type" => "application/json"));
$req->getBody()->append (json_encode ($deposit_permission));

// Execute the HTTP request
$client = new http\Client;
$client->enqueue($req)->send();

// Fetch the response
$resp = $client->getResponse();
$status_code = $resp->getResponseCode();

// Our response code is the same we got from the backend:
http_response_code($status_code);

// Now generate our body  
if ($status_code != 200)
{
  $json = json_encode(
    array(
      "error" => "backend error",
      "status" => $status_code,
      "detail" => $resp->body->toString()));
  echo $json;
  die();
}

session_start();

$payments = &pull($_SESSION, "payments", array());
$payments[$hc] = array(
  'receiver' => $receiver,
  'is_payed' => true
);

?>