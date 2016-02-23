<?php

$REFUND_DELTA = 'P3M';
// set to false when done with local tests
$explicit_currency = "PUDOS";
$MERCHANT_CURRENCY = $explicit_currency;

$host = $_SERVER["HTTP_HOST"];
switch ($host) {
case "blog.demo.taler.net":
case "shop.demo.taler.net":
  $MERCHANT_CURRENCY = "KUDOS";
  break;
case "blog.test.taler.net":
case "shop.test.taler.net":
  $MERCHANT_CURRENCY = "PUDOS";
  break;
default:
  if(false == $explicit_currency){
    http_response_code (500);
    echo "<p>Bank configuration error: No currency for domain $host</p>\n";
    die();
  }
}
?>
