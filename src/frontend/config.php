<?php

$host = $_SERVER["HTTP_HOST"];

switch ($host) {
case "shop.demo.taler.net":
  $SHOP_CURRENCY = "KUDOS";
  break;
case "shop.test.taler.net":
  $SHOP_CURRENCY = "PUDOS";
  break;
default:
  http_response_code ($status_code);
  echo "<p>Configuration error: No currency for domain $host</p>\n";
  die();
  break;
}

?>
