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
 * 2. augment the deposit permission with missin values
 * 3. forward payment to backend
 */

if (!isset($_SESSION['H_contract']))
{
  echo "No session active.";
  http_response_code (301);
  return;
}

$article = $_SESSION['article'];
$post_body = file_get_contents('php://input');
$deposit_permission = json_decode ($post_body, true);
$to_add = array('max_fee' => array('value' => 3,
                                   'fraction' => 8,
                                   'currency' => $_SESSION['currency']),
                'amount' => array('value' => $_SESSION['article_value'],
                                  'fraction' => $_SESSION['article_fraction'],
		                  'currency' => $_SESSION['article_currency']));
$complete_deposit_permission = array_merge($deposit_permission, $to_add);
$resp = give_to_backend($_SERVER['HTTP_HOST'],
                        "backend/pay",
			$complete_deposit_permission);
$status_code = $resp->getResponseCode();
// Our response code is the same we got from the backend:
http_response_code ($status_code);

// Now generate our body  
if ($status_code != 200)
{
  /* error: just forwarding to the wallet what
    gotten from the backend (which is forwarding 'as is'
    the error gotten from the mint) */
  echo json_encode ($new_deposit_permission);
  echo "Error came from the backend, payment undone. Status $status_code\n";
  echo "\n";
  echo $resp->body->toString ();
}
else
{
  $_SESSION['payment_ok'] = true;
  if (!isset($_SESSION['allowed_articles'])){
    $_SESSION['allowed_articles'] = array ($_SESSION['article'] => true);
  else $_SESSION['allowed_articles'] =
    array_push($_SESSION['allowed_articles'], array ($article => true));
  }

  http_response_code (301);
  $url = (new http\URL($_SERVER['REQUEST_SCHEME'] . '://' . $_SERVER['HTTP_HOST'].$_SERVER['REQUEST_URI']))
    ->mod(array ("path" => "essay_fulfillment.php?article=$article"), http\Url::JOIN_PATH);
  header("Location: $url");
  die();
}

