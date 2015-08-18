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
        General Public License (GNU GPL) as published by the Free Software
        Foundation, either version 3 of the License, or (at your option)
        any later version.  The code is distributed WITHOUT ANY WARRANTY;
        without even the implied warranty of MERCHANTABILITY or FITNESS
        FOR A PARTICULAR PURPOSE.  See the GNU GPL for more details.

        As additional permission under GNU GPL version 3 section 7, you
        may distribute non-source (e.g., minimized or compacted) forms of
        that code without the copy of the GNU GPL normally required by
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
  
  1. retrieve the name of who will receive this donation
  2. show a menu with all the required payments means
  3. create a session
  4. (JavaScript) implement the "checkout" button for the sole Taler way.
  Actually, this button's duty is to notice this web portal that the customer
  wants to see a certificate, and optionally to pay.

  -->

<?php

// getting the donation receiver's name
$got_donation = $_POST['group0'];
// create session
session_start();
$_SESSION['maydonate'] = true;
?>

<form name="tform" action="" method="POST">
<div id="opt-form" align="left"><br>
<input type="radio" name="group1" value="Lisa">Lisa<br>
<input type="radio" name="group1" value="You Card" checked>You Card<br>
<input type="radio" name="group1" value="Card Me">Card Me<br>
<input id="t-button-id" type="radio" name="group1" value="Taler" disabled="true">Taler<br>
<input type="button" onclick="ok(this.form)" value="Ok">
</div>
</form>

<script type="text/javascript">

// trigger certificate generation on the server, and signal certificate
// arrival "here" in the client.

function ok(form){
  for(var cnt=0; cnt < form.group1.length; cnt++){
    var choice = form.group1[cnt];
      if(choice.checked){
        if(choice.value == "Taler"){
          var cert = new XMLHttpRequest();
          /* request certificate */
          cert.open("POST", "/cert.php", true);
          cert.onload = function (e) {
            if (cert.readyState == 4) {
              if (cert.status == 200){
              /* display certificate (i.e. it sends the JSON string
              to the (XUL) extension) */      
              sendContract(cert.responseText);
              }
              else alert("No certificate gotten, status " + cert.status);
            }
          };

          cert.onerror = function (e){
            alert(cert.statusText);
          };

          cert.send(null);
        }
        else alert(choice.value + ": NOT available ");
      }
  }
};




/* the following event gets fired whenever a customer has a Taler
   wallet installed in his browser. In that case, the webmaster can decide
   whether or not displaying Taler as a payment option */

function has_taler_wallet_cb(aEvent){
  // event awaited by the wallet to change its button's color
  var eve = new Event('taler-currency');
  document.body.dispatchEvent(eve);

  // enable the Taler payment option from the form
  var tbutton = document.getElementById("t-button-id");
  tbutton.removeAttribute("disabled");
};


// The Taler extension was unloaded, disable the option
function taler_wallet_unload_cb(aEvent){
  var tbutton = document.getElementById("t-button-id");
  tbutton.addAttribute("disabled");
};

  
function sendContract(jsonContract){

  var cevent = new CustomEvent('taler-contract', { 'detail' : jsonContract });
  document.body.dispatchEvent(cevent);
};

/* FIXME: these triggers do not work when I enable/disable
   the extension... */
// to be triggered by the wallet
document.body.addEventListener("taler-wallet", has_taler_wallet_cb, false);

// to be triggered by the wallet when it is unloaded
document.body.addEventListener("taler-unload", taler_wallet_unload_cb, false);


</script>
</body>
</html>
