<!DOCTYPE html>
<html>
<head>
<title>Choose payment method</title>
    <script>
        /*
        @licstart  The following is the entire license notice for the
        JavaScript code in this page.

        Copyright (C) 2014,2015 GNUnet e.V.

        The JavaScript code in this page is free software: you can
        redistribute it and/or modify it under the terms of the GNU
        Lesser General Public License (GNU LGPL) as published by the Free Software
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
<body>
<!-- 
  This page's main aim is to show to the customer all the accepted
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

  // create PHP session and store donation information in session
  session_start();
  $_SESSION['receiver'] = $donation_receiver;
  $_SESSION['amount'] = $donation_amount;
?>

<form name="tform" action="" method="POST">
  <div id="opt-form" align="left"><br>
    <input type="radio" name="payment_system" value="lisa" checked>Lisa</input>
    <br>
    <input type="radio" name="payment_system" value="ycard">You Card</input>
    <br>
    <input type="radio" name="payment_system" value="cardme">Card Me</input>
    <br>
    <input type="radio" name="payment_system" value="taler" 
          id="taler-radio-button-id" disabled="true">Taler</input>
    <br>
    <input type="button" onclick="pay(this.form)" value="Ok">
  </div>
</form>

<script type="text/javascript">

/* We got a JSON contract from the merchant,
   pass it to the extension */
function handle_contract(json_contract)
{
  var cevent = new CustomEvent('taler-contract', { 'detail' : jsonContract });
  document.body.dispatchEvent(cevent);
};


/* Trigger Taler contract generation on the server, and pass the
   contract to the extension once we got it. */
function taler_pay(form)
{
  var contract_request = new XMLHttpRequest();
  contract_request.open("POST", "/generate_taler_contract.php", true);
  contract_request.onload = function (e) 
  {
    if (contract_request.readyState == 4) 
    {
      if (contract_request.status == 200)
      {
        /* display contract_requestificate (i.e. it sends the JSON string
           to the (XUL) extension) */      
        handle_contract(contract_request.responseText);
      }
      else 
      {
        alert("Failed to receive contract from server. Status was " + contract_request.status);
      }
    }
  };
  contract_request.onerror = function (e)
  {
    alert(contract_request.statusText);
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
   whether or not displaying Taler as a payment option */
function has_taler_wallet_cb(event)
{
  // event awaited by the wallet to change its button's color
  var eve = new Event('taler-currency');
  document.body.dispatchEvent(eve);

  // enable the Taler payment option from the form
  var tbutton = document.getElementById("taler-radio-button-id");
  tbutton.removeAttribute("disabled");
};


/* Function called when the Taler extension was unloaded, 
   here we disable the option */
function taler_wallet_unload_cb(event)
{
  var tbutton = document.getElementById("taler-radio-button-id");
  tbutton.addAttribute("disabled");
};


/* FIXME: these triggers do not work when I enable/disable
   the extension... */
// Register event to be triggered by the wallet (when present)
document.body.addEventListener("taler-wallet", has_taler_wallet_cb, false);

// Register event to be triggered by the wallet when it is unloaded
document.body.addEventListener("taler-unload", taler_wallet_unload_cb, false);

</script>
</body>
</html>
