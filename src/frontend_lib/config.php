<?php

$explicit_currency = false;
//$explicit_currency = "PUDOS";

$host = $_SERVER["HTTP_HOST"];
switch ($host) {
case "blog.demo.taler.net":
case "show.demo.taler.net":
  $MERCHANT_CURRENCY = "KUDOS";
  break;
case "blog.test.taler.net":
case "shop.test.taler.net":
  $MERCHANT_CURRENCY = "PUDOS";
  break;
default:
  if ($explicit_currency != false)
    $MERCHANT_CURRENCY = $explicit_currency;
  else {
  http_response_code (500);
  echo "<p>Bank configuration error: No currency for domain $host</p>\n";
  die();
  }
}
?>
