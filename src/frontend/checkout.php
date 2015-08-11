<html>
<head>
<title>Choose payment method</title>
</head>
<body>

<!--

  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>

-->

<!-- This page's main aim is to show to the customer all the accepted payments methods
  and actually implementing just Taler; technically the steps are:
  


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




/* the following event gets fired whenever a customer has a taler
wallet installed in his browser. In that case, the webmaster can decide
whether or not displaying Taler as a payment option */

function hasWallet(aEvent){
  // event awaited by the wallet to change its button's color
  var eve = new Event('taler-currency');
  document.body.dispatchEvent(eve);

  // ungrey the Taler payment option from the form
  var tbutton = document.getElementById("t-button-id");
  tbutton.removeAttribute("disabled");
};


  
function sendContract(jsonContract){

  var cevent = new CustomEvent('taler-contract', { 'detail' : jsonContract });
  document.body.dispatchEvent(cevent);
};

function closeEnd(aEvent){
  
  var eve = new Event("taler-unload");
  document.body.dispatchEvent(eve);

};

// to be triggered by the wallet
document.body.addEventListener("taler-wallet", hasWallet, false);
// to be triggered by the wallet
document.body.addEventListener("taler-unload", closeEnd, false);


</script>
</body>
</html>
