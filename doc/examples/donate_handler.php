<?php
  http_response_code (402); // 402: Payment required
  header ('X-Taler-Contract-Url: /generate-contract');
?>
<html>
  <head>
  <title>Select payment method</title>
  </head>
  <body>
    Here you should put the HTML for the non-Taler (credit card) payment.
  </body>
</html>
