<?php
  http_response_code (402); // 402: Payment required
  header ('X-Taler-Contract-Url: /generate-contract');
?>
<html>
  <head>
  <title>Select payment method</title>
  </head>
  <body>
    <!-- Put the credit card paywall here -->
  </body>
</html>
