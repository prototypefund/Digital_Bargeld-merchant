<!DOCTYPE html>
<!--
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
-->
<html lang="en">
<head>
  <title>Taler's "Demo" Shop</title>
  <link rel="stylesheet" type="text/css" href="style.css">
  <script type="application/javascript" src="taler-presence.js"></script>
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
  </script>
</head>
<body>
<!--
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">S</text>
      </svg>
    </div>

    <h3>Toy Store - Product Page</h3>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
  -->
<?php
// TODO return a mock CC payment page if no wallet in place
include '../frontend_lib/util.php';
include './blog_lib.php';

$article = get($_GET['article']);
if (null == $article){
  http_response_code(400);
  echo "<p>Bad request (article missing)</p>";
  return;
}
$hc = get($_GET['uuid']);
if (null == $article){
  http_response_code(400);
  echo "<p>Bad request (UUID missing)</p>";
  return;
}

session_start();

$payments = get($_SESSION['payments'], array());
$my_payment = get($payments[$hc]);

$pay_url = url_rel("essay_pay.php");
$offering_url = url_rel("essay_offer.php", true);
$offering_url .= "?article=$article";
file_put_contents("/tmp/essay_pay-offer", "pay URL:" . $payurl . "\noffer URL:" . $offering_url);
if (true !== get($my_payment["is_payed"], false) || null === $my_payment)
    
{
  $tid = get($_GET['tid']);
  $timestamp = get($_GET['timestamp']);
  // FIXME article name should be "melted" in the hash
  // TODO reconstruct *here* the contract, hash it, and save it in the state

  if (null == $tid || null == $timestamp){
    // CC case
    $_SESSION['cc_payment'] = true;
    $cc_page = template("./essay_cc-payment.html", array('article' => $article));
    echo $cc_page;
    die();
  
  }
  echo "<p>Paying ... at $pay_url </p>";
  echo "<script>executePayment('$hc', '$pay_url', '$offering_url');</script>";
  return;
}

// control here == article payed

$article = $my_payment["article"];

$article_doc = get_article($article);
echo $article_doc->saveHTML();

?>
    <!--
    </article>
  </section>
  -->
</body>
</html>
