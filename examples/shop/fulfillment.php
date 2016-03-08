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
  <link rel="stylesheet" type="text/css" href="style.css">
  <script type="application/javascript" src="web-common/taler-presence.js"></script>
  <script type="application/javascript">
  function executePayment(H_contract, pay_url, offering_url) {
    var detail = {
      H_contract: H_contract,
      pay_url: pay_url,
      offering_url: offering_url
    };
    var eve = new CustomEvent('taler-execute-payment', {detail: detail});
    document.dispatchEvent(eve);
  }
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

    <h1>Toy Store - Product Page</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
<?php

include '../../copylib/util.php';

$hc = get($_GET["uuid"]);

if (empty($hc)) {
  http_response_code(400);
  echo "<p>Bad request (UUID missing)</p>";
  return;
}

session_start();

$payments = get($_SESSION['payments'], array());
$my_payment = get($payments[$hc]);

// This will keep the query parameters.
$pay_url = url_rel("pay.php");

$offering_url = url_rel("checkout.php", true);

if (null === $my_payment) {
  // TODO: show spinner after timeout
  echo "<p>you do not have the session state for this contract: " . $hc . "</p>";
  echo "<p>Asking the wallet to re-execute it ... </p>";
  echo "<script>executePayment('$hc', '$pay_url', '$offering_url');</script>";
  return;
}

if (true !== get($my_payment["is_payed"], false)) {
  // TODO: show spinner after timeout
  echo "<p>you have not payed for this contract: " . $hc . "</p>";
  echo "<p>Asking the wallet to re-execute it ... </p>";
  echo "<script>executePayment('$hc', '$pay_url');</script>";
  return;
}

$receiver = $my_payment["receiver"];

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
