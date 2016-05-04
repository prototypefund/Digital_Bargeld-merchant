<!DOCTYPE html>
<!-- 
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
-->
<html lang="en">
<head>
  <title>Taler's "Demo" Shop</title>
  <link rel="stylesheet" type="text/css" href="web-common/style.css">
  <script type="application/javascript" src="web-common/taler-wallet-lib.js"></script>
  <script type="application/javascript">
  function makeVisible() {
    function cb() {
      document.body.style.display = "";
    }
    document.addEventListener("DOMContentLoaded", cb, false);
  }
  </script>
</head>
<body style="display:none;"> 
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">S</text>
      </svg>
    </div>
    <!--#include virtual="web-common/dropdown-navbar.html"-->
    <h1 class="nav">Toy Store - Product Page</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
<?php

include '../../copylib/util.php';
include "../../copylib/merchants.php";

$receiver = get($_GET["receiver"]);
$now = new DateTime();
$now->setTimestamp(intval(get($_GET["timestamp"])));

if (empty($receiver)) {
  http_response_code(400);
  echo "<p>Bad request (UUID missing)</p>";
  return;
}

session_start();

$payments = &pull($_SESSION, 'payments', array());
$my_payment = &pull($payments, $receiver, array());

// This will keep the query parameters.
$pay_url = url_rel("pay.php");
$offering_url = url_rel("index.php", true);

if (array() === $my_payment || true !== get($my_payment["is_payed"], false)) {
  // restore contract
  $contract = generate_contract(array(
    "amount_value" => intval($_GET['aval']),
    "amount_fraction" => intval($_GET['afrac']),
    "currency" => $_GET['acurr'],
    "refund_delta" => 'P3M',
    "transaction_id" => intval($_GET['tid']),
    "description" => "Donation to " . $receiver,
    "product_id" => "unused",
    "correlation_id" => "",
    "merchant_name" => "Kudos Inc.",
    "taxes" => array(),
    "now" => $now,
    "fulfillment_url" => get_full_uri())
  );
  
  $resp = give_to_backend("backend/contract", $contract);
  if ($resp->getResponseCode() != 200){
    echo json_encode(array(
    'error' => "internal error",
    'hint' => "failed to regenerate contract",
    'detail' => $resp->body->toString()
    ), JSON_PRETTY_PRINT);
    return;
  }

  $hc = json_decode($resp->body->toString(), true)['H_contract'];
  $my_payment['is_payed'] = false;
  $my_payment['hc'] = $hc;
  echo "<p>you have not payed for this contract: " . $hc . "</p>";
  echo "<p>Asking the wallet to re-execute it ... </p>";
  echo "<script>taler.executePayment('$hc', '$pay_url', '$offering_url');</script>";
  return;
}

$news = false;
switch ($receiver) {
  case "Taler":
    $news = "https://taler.net/news";
    break;
  case "GNUnet":
    $news = "https://gnunet.org/";
    break;
  case "Tor":
    $news = "https://www.torproject.org/press/press.html.en";
    break;
}

$msg = "<p>Thanks for donating to " . $receiver . ".</p>";
if ($news) {
  $msg .= "<p>Check our latest <a href=\"" . $news . "\">news!</a></p>";
}

echo $msg;

echo "<script>makeVisible();</script>";

?>
    </article>
  </section>
</body>
</html>
