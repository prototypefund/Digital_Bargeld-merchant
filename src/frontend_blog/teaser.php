<html>
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
 *
 * 1. fetch the article teaser and attach a 'read more' link to it
 *
 */
$article = (isset($_GET['article']) ? $_GET['article'] : "No article");
$content = file_get_contents("articles/$article.html");
$doc = new DOMDocument();
$doc->loadHTML($content);
$teaser = $doc->getElementById("teaser");
?>
<head>
  <title>
    <?php echo $article ?>
  </title>
</head>
<body onload="signal_taler_wallet_onload();">
<?php if ($article == "No article")
        echo "Please select some article";
      else {
        session_start();
	$_SESSION['article'] = $article;
	echo $teaser->nodeValue;
      }
?>
	<br><a href="cc_payment.html" id="read-more">read more</a>

</body>
<script>
/* This function is called from "taler_pay" after
   we downloaded the JSON contract from the merchant.
   We now need to pass it to the extension. */
function handle_contract(json_contract)
{
  var cEvent = new CustomEvent('taler-contract', { detail: json_contract });

  document.dispatchEvent(cEvent);
};

function taler_pay()
{
  var contract_request = new XMLHttpRequest();

  /* Note that the URL we give here is specific to the Demo-shop
     and not required by the protocol: each web shop can
     have its own way of generating and transmitting the
     contract, there just must be a way to get the contract
     and to pass it to the wallet when the user selects 'Pay'. */
  contract_request.open("GET", "essay_contract.php", true);
  contract_request.onload = function (e)
  {
    if (contract_request.readyState == 4)
    {
      if (contract_request.status == 200)
      {
        /* display contract_requestificate (i.e. it sends the JSON string
          to the extension) alert (contract_request.responseText); */
	console.log("contract here");
        console.log("response text:", contract_request.responseText);
        handle_contract(contract_request.responseText);
      }
      else
      {
        /* There was an error obtaining the contract from the merchant,
           obviously this should not happen. To keep it simple, we just
           alert the user to the error. */
        alert("Failure to download contract from merchant " +
              "(" + contract_request.status + "):\n" +
              contract_request.responseText);
      }
    }
  };
  contract_request.onerror = function (e)
  {
    /* There was an error obtaining the contract from the merchant,
       obviously this should not happen. To keep it simple, we just
       alert the user to the error. */
    alert("Failure requesting the contract:\n" + contract_request.statusText);
  };
  contract_request.send(null);
}

/* The following event gets fired whenever a customer has a Taler
   wallet installed in his browser. In that case, the webmaster can decide
   whether or not to display/enable Taler as a payment option in the dialog. */
function has_taler_wallet_cb(aEvent)
{
  console.log("has taler wallet");
  // make "read more" trigger Taler payment
  var rm = document.getElementById("read-more");
  rm.setAttribute("href", "javascript:taler_pay();");
};

/* Function called when the Taler extension was unloaded;
   here we disable the Taler option and check "Lisa", as
   some "valid" option should always be selected. */
function taler_wallet_unload_cb(aEvent)
{
  var rm = document.getElementById("read-more");
  rm.setAttribute("href", "cc_payment.html");
};


/* The merchant signals its taler-friendlyness to the wallet,
   thereby causing the wallet to make itself more visible in the menu.
   This function should be called both when the page is loaded
   (i.e. via body's onload) and when we receive a "taler-load" signal
   (as the extension may be loaded/enabled after the page was loaded) */
function signal_taler_wallet_onload()
{
  var eve = new Event('taler-probe');
  document.dispatchEvent(eve);
};


// function included to be run to test the page despite a
// wallet not being present in the browser.  Enables the
// Taler option. NOT needed in real deployments.
function test_without_wallet(){
  var tbutton = document.getElementById("taler-radio-button-id");
  tbutton.removeAttribute("disabled");
};


// /////////////// Main logic run first ////////////////////////

// Register event to be triggered by the wallet as a response to our
// first event
document.addEventListener("taler-wallet-present",
                          has_taler_wallet_cb,
			  false);

// Register event to be triggered by the wallet when it gets enabled while
// the user is on the payment page
document.addEventListener("taler-load",
                          signal_taler_wallet_onload,
			  false);

// Register event to be triggered by the wallet when it is unloaded
document.addEventListener("taler-unload",
                          taler_wallet_unload_cb,
	                  false);
</script>
</html>
