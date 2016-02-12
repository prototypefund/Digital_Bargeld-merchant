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

/**
 * This file should:
 * 1. Check if the session is valid
 * 2. augment the deposit permission with missing values
 * 3. forward payment to backend
 */
include("../frontend_lib/merchants.php");
include("../frontend_lib/util.php");
include("./blog_lib.php");

session_start();

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
$article = get($_GET["article"]);
if (empty($article))
{
  http_response_code(400);
  echo json_encode(array(
    "error" => "missing parameter",
    "parameter" => "article"
  ));
  return;
}

$deposit_permission = file_get_contents('php://input');
file_put_contents('/tmp/pay.dbg', 'about to pay\n', FILE_APPEND);
$resp = give_to_backend($_SERVER['HTTP_HOST'],
                        "backend/pay",
			$deposit_permission);
file_put_contents('/tmp/pay.dbg', 'backend respd\n', FILE_APPEND);
$status_code = $resp->getResponseCode();

// Our response code is the same we got from the backend:
http_response_code ($status_code);
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
  'article' => $article,
  'is_payed' => true
);
?>
