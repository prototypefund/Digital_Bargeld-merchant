<!DOCTYPE html>
<html>
<head>
  <title>Toy Store - Payment method - Taler Demo</title>
  <link rel="stylesheet" type="text/css" href="style.css">
    <script>
        /*
        @licstart  The following is the entire license notice for the
        JavaScript code in this page.

        Copyright (C) 2014,2015 GNUnet e.V.

        The JavaScript code in this page is free software: you can
        redistribute it and/or modify it under the terms of the GNU
        Lesser General Public License (GNU LGPL) as published by the
	Free Software
        Foundation, either version 3 of the License, or (at your option)
        any later version.  The code is distributed WITHOUT ANY WARRANTY;
        without even the implied warranty of MERCHANTABILITY or FITNESS
        FOR A PARTICULAR PURPOSE.  See the GNU LGPL for more details.

        As additional permission under GNU LGPL version 3 section 7, you
        may distribute non-source (e.g., minimized or compacted) forms of
        that code without the copy of the GNU LGPL normally required by
        section 4, provided you include this license notice and a URL
        through which recipients can access the Corresponding Source.

        @licend  The above is the entire license notice
        for the JavaScript code in this page.
        */
    </script>
</head>
<body onload="signal_taler_wallet_onload()">
<!--
  This main goal of this page is to show to the customer all the accepted
  payments methods and actually implementing just Taler; technically
  the steps are:

  1. retrieve the information about the donation from the
     form and remember it in a PHP session
  2. show a menu with all the required payments system options,
     only showing "Taler" if the wallet is present
  3. (JavaScript) implement the "checkout" button for Taler,
     for the demo we ignore the other payment options.
-->

<?php
  // get the donation information from form
  $donation_receiver = $_POST['donation_receiver'];
  $donation_amount = $_POST['donation_amount'];
  $donation_currency = $_POST['donation_currency'];

  // get frational part
  list ($donation_value, $donation_fraction) = explode (".", $donation_amount, 2);
  // create PHP session and store donation information in session
  $donation_fraction = (float) ("0." . $donation_fraction);
  session_start();
  session_unset();
  $_SESSION['receiver'] = $donation_receiver;
  $_SESSION['amount_value'] = (int) $donation_amount;
  $_SESSION['amount_fraction'] = (int) ($donation_fraction * 1000000);
  $_SESSION['currency'] = $donation_currency;
?>

  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">S</text>
      </svg>
    </div>

    <h1>Toy Store - Select payment method</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>

      <h1>Select your payment method</h1>

      <p>
        This is an example for a "checkout" page of a Web shop.
        On the previous page, you have created the shopping cart
        and decided which product to buy (i.e. which project to
        donate KUDOS to).  Now in this page, you are asked to
        select a payment option.  As Taler is not yet universally
        used, we expect merchants will offer various payment options.
      </p>
      <p>
        The page also demonstrates how to only enable (or show) the Taler
        option if Taler is actually supported by the browser.  For example,
        if you disable the Taler extension now, the Taler payment option
        will be disabled in the page.  Naturally, you could also trivially
        hide the Taler option entirely by changing the visibility instead.
      </p>
      <p>
        Note that you MUST select Taler here for the demo to continue,
        as the other payment options are just placeholders and not
        really working in the demonstration.  Also, it is of course
        possible to ask the user to make this choice already on the
        previous page (with the shopping cart), we just separated the
        two steps to keep each step as simple as possible.
      </p>

      <form name="tform" action="" method="POST">
        <div id="opt-form" align="left"><br>
          <input type="radio" name="payment_system" value="lisa"
                 id="lisa-radio-button-id">Lisa</input>
          <br/>
          <input type="radio" name="payment_system" value="ycard">You Card</input>
          <br/>
          <input type="radio" name="payment_system" value="cardme">Card Me</input>
          <br/>
          <input type="radio" name="payment_system" value="taler"
                 id="taler-radio-button-id" disabled="true">Taler</input>
          <br/>
          <input type="button" onclick="pay(this.form)" value="Ok"></input>
        </div>
      </form>

    </article>
  </section>

<script type="text/javascript">

/* This function is called from "taler_pay" after
   we downloaded the JSON contract from the merchant.
   We now need to pass it to the extension. */
function handle_contract(json_contract)
{
  var cEvent = new CustomEvent('taler-contract', { detail: json_contract });

  document.dispatchEvent(cEvent);
};


/* Trigger Taler contract generation on the server, and pass the
   contract to the extension once we got it. */
function taler_pay(form)
{
  var contract_request = new XMLHttpRequest();

  /* Note that the URL we give here is specific to the Demo-shop
     and not required by the protocol: each web shop can
     have its own way of generating and transmitting the
     contract, there just must be a way to get the contract
     and to pass it to the wallet when the user selects 'Pay'. */
  contract_request.open("GET", "generate_taler_contract.php", true);
  contract_request.onload = function (e)
  {
    if (contract_request.readyState == 4)
    {
      if (contract_request.status == 200)
      {
        /* display contract_requestificate (i.e. it sends the JSON string
          to the extension) alert (contract_request.responseText); */
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


/* This function is called when the user presses the
   'Ok' button.  We are now supposed to trigger the
   "corret" payment system logic. For this demo, we
   only handle "taler". */
function pay(form)
{
  for (var cnt=0; cnt < form.payment_system.length; cnt++)
  {
    var choice = form.payment_system[cnt];
    if (choice.checked)
    {
       if (choice.value == "taler")
       {
         taler_pay(form);
       }
       else
       {
         alert(choice.value + ": NOT available in this demo!");
       }
    }
  }
};


/* The following event gets fired whenever a customer has a Taler
   wallet installed in his browser. In that case, the webmaster can decide
   whether or not to display/enable Taler as a payment option in the dialog. */
function has_taler_wallet_cb(aEvent)
{
  // enable the Taler payment option from the form
  var tbutton = document.getElementById("taler-radio-button-id");
  tbutton.removeAttribute("disabled");
  tbutton.setAttribute("checked", "true");
};


/* Function called when the Taler extension was unloaded;
   here we disable the Taler option and check "Lisa", as
   some "valid" option should always be selected. */
function taler_wallet_unload_cb(aEvent)
{
  var tbutton = document.getElementById("taler-radio-button-id");
  tbutton.setAttribute("disabled", "true");
  var lbutton = document.getElementById("lisa-radio-button-id");
  lbutton.setAttribute("checked", "true");
};


/* The merchant signals its taler-friendlyness to the wallet,
   thereby causing the wallet to make itself more visible in the menu.
   This function should be called both when the page is loaded
   (i.e. via body's onload) and when we receive a "taler-load" signal
   (as the extension may be loaded/enabled after the page was loaded) */
function signal_taler_wallet_onload()
{
  var eve = new Event('taler-checkout-probe');
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
</body>
</html>
