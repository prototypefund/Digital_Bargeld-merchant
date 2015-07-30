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

<!-- This page has to:
  
  1. make known to the customer this transaction ID

  2. Generate (but still NOT sending) the relevant certificate
  
  3. (JavaScript) implement the Pay button for the sole Taler way.
  Actually, this button's duty is just to ask for the (already generated)
  certificate.

  4. (JavaScript) request the certificate associated with this
  ID, through "GET /certal/"
  
  
  -->

<?php

  // ID generation
  $transId = rand(1, 15);

  // embedding trans ID in a hidden input HTML tag
  //echo "<input type=\"hidden\" id=\"taler-trans-id\" value=\"$transId\" />";

  // JSON certificate generation matching the product being sold
  $item = $_POST['group0'];
  
  $toJSON = array('vendor' => "$item provider", 'item' => $item, 'price'=> rand(5, 66) . ' €', 'payUrl' => "http://" . $_SERVER['SERVER_NAME'] . "/payler/");  


  // save certificate (retrievable through file naming convention) to the disk
  // file_put_contents(getcwd() . "/cert." . $transId, json_encode($toJSON));
  
  // time-expirable (15') tracking cookie definition
  // setcookie("talkie", $transId, time()+ 15*60);

  // create session

  session_start();
  $_SESSION['contract'] = json_encode($toJSON);




?>

<form name="tform" action="" method="POST">
<div id="opt-form" align="left"><br>
<input type="radio" name="group1" value="Lisa">Lisa<br>
<input type="radio" name="group1" value="You Card" checked>You Card<br>
<input type="radio" name="group1" value="Card Me">Card Me<br>
<input id="t-button-id" type="radio" name="group1" value="Taler" disabled="true">Taler<br>
<input type="button" onclick="pay(this.form)" value="Ok">
</div>
</form>

<script type="text/javascript">

 function pay(form){
    for(var cnt=0; cnt < form.group1.length; cnt++){
      var choice = form.group1[cnt];
      if(choice.checked){
        if(choice.value == "Taler"){
          var cert = new XMLHttpRequest();
          /* request certificate */
          cert.open("GET", "certal/", true);
          cert.onload = function (e) {
            if (cert.readyState == 4) {
                if (cert.status == 200){
                /* display certificate (i.e. it sends the JSON string
              to the (XUL) extension) */      
              sendContract(cert.responseText);
                }
                else alert("Certificate ready state: " + cert.readyState + ", cert status: " + cert.status);
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
    
  var eve = new Event('taler-currency');
        document.body.dispatchEvent(eve);

  /* old way of generating events ; left here in case of portability issues*/

  /*var tevent = document.createEvent("Events");
  tevent.initEvent("taler-currency", true, false);
        document.body.dispatchEvent(tevent);*/


  /* embedding Taler's availability information inside the form containing
  items to be paid */
  var tbutton = document.getElementById("t-button-id");
  tbutton.removeAttribute("disabled");
  };


  
  function sendContract(jsonContract){

  var cevent = new CustomEvent('taler-contract', { 'detail' : jsonContract });
        document.body.dispatchEvent(cevent);



  /* old way of generating events ; left here in case of portability issues*/

  /*var cevent = document.createEvent("Events");
  cevent.initEvent("taler-contract", true, false);
        document.body.dispatchEvent(cevent);*/
  

  
  };

  function closeEnd(aEvent){
  
  var eve = new Event("taler-unload");
        document.body.dispatchEvent(eve);

  };

  document.body.addEventListener("taler-wallet", hasWallet, false);
  document.body.addEventListener("taler-shutdown", closeEnd, false);


</script>



</body>

</html>
