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
  <title>Taler's "Demo" Blog</title>
  <link rel="stylesheet" type="text/css" href="style.css"-->
</head>
<body>
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">B</text>
      </svg>
    </div>

    <h1>Taler's "Demo" Blog</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
<?php
include '../frontend_lib/util.php';
include '../frontend_lib/merchants.php';
include '../frontend_lib/config.php';
include './blog_lib.php';

$article = get($_GET['article']);
if (null == $article){
  http_response_code(400);
  echo "<p>Bad request (article missing)</p>";
  return;
}

session_start();

$payments = get($_SESSION['payments'], array());
$my_payment = get($payments[$article]);

$pay_url = url_rel("essay_pay.php");
$offering_url = url_rel("essay_offer.php", true);
$offering_url .= "?article=$article";
if ("payed" != $my_payment || null === $my_payment)
    
{
  $tid = get($_GET['tid']);
  $timestamp = get($_GET['timestamp']);
  // 1st time
  if (null == $tid || null == $timestamp){
    $js_code = "get_contract(\"$article\")";
    $cc_page = template("./essay_cc-payment.html", array('article' => $article, 'jscode' => $js_code));
    echo $cc_page;
    die();
  }
  // restore
  $now = new DateTime();
  $now->setTimestamp(intval($timestamp));
  
  $contract_rec = generate_contract(0,
                                    50000,
				    $MERCHANT_CURRENCY,
				    intval($tid),
				    trim(get_teaser($article)->nodeValue),
				    $article,
				    $article,
				    array(),
				    $now,
				    get_full_uri());

  $resp = give_to_backend($_SERVER['HTTP_HOST'],
                          "backend/contract",
                          $contract_rec);
  if ($resp->getResponseCode() != 200)
  {
    echo json_encode(array(
      'error' => "internal error",
      'hint' => "non hashable contract",
      'detail' => $resp->body->toString()
    ), JSON_PRETTY_PRINT);
    die();
  }
  $hc = json_decode($resp->body->toString(), true)['H_contract'];
  $js_code = "executePayment('$hc', '$pay_url', '$offering_url')";
  $cc_page = template("./essay_cc-payment.html", array('article' => $article, 'jscode' => $js_code));
  echo $cc_page;
  return;
  }

// control here == article payed

$article_doc = get_article($article);
echo $article_doc->saveHTML($article_doc->getElementById("full-article"));
?>
    </article>
  </section>
</body>
  <script type="application/javascript" src="taler-presence.js"></script>
  <script type="application/javascript">
  function handle_contract(json_contract) {
    var cEvent = new CustomEvent('taler-contract',
                                 {detail: json_contract});
    document.dispatchEvent(cEvent);
  };

  function get_contract(article) {
  var contract_request = new XMLHttpRequest();

  contract_request.open("GET",
                        "essay_contract.php?article=" + article,
			true);
  contract_request.onload = function (e) {
    if (contract_request.readyState == 4) {
      if (contract_request.status == 200) {
        console.log("response text:",
	            contract_request.responseText);
        handle_contract(contract_request.responseText);
      } else {
        alert("Failure to download contract from merchant " +
              "(" + contract_request.status + "):\n" +
              contract_request.responseText);
      }
    }
  };
  contract_request.onerror = function (e) {
    alert("Failure requesting the contract:\n"
          + contract_request.statusText);
  };
  contract_request.send();
  }

  function has_taler_wallet_cb(aEvent)
  {
    var article = document.getElementById('article-name');
    get_contract(article.value); 
  };
  
  function signal_taler_wallet_onload()
  {
    var eve = new Event('taler-probe');
    document.dispatchEvent(eve);
  };
  
  document.addEventListener("taler-wallet-present",
                            has_taler_wallet_cb,
  			  false);
  
  // Register event to be triggered by the wallet when it gets enabled while
  // the user is on the payment page
  document.addEventListener("taler-load",
                            signal_taler_wallet_onload,
                            false);




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

</html>
