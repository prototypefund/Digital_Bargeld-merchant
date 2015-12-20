<!DOCTYPE html>
<html lang="en">
<head>
  <title>Toy Store - Taler Demo</title>
  <link rel="stylesheet" type="text/css" href="style.css">
    <script> /* @licstart  The following is the entire license notice for the
        JavaScript code in this page.

        Copyright (C) 2015 GNUnet e.V.

        The JavaScript code in this page is free software: you can
        redistribute it and/or modify it under the terms of the GNU
        Lesser General Public License (GNU LGPL) as published by the Free Software
        Foundation, either version 2.1 of the License, or (at your option)
        any later version.  The code is distributed WITHOUT ANY WARRANTY;
        without even the implied warranty of MERCHANTABILITY or FITNESS
        FOR A PARTICULAR PURPOSE.  See the GNU LGPL for more details.

        As additional permission under GNU LGPL version 2.1 section 7, you
        may distribute non-source (e.g., minimized or compacted) forms of
        that code without the copy of the GNU LGPL normally required by
        section 4, provided you include this license notice and a URL
        through which recipients can access the Corresponding Source.

        @licend  The above is the entire license notice
        for the JavaScript code in this page.
        */
    </script>
    <script type="text/javascript">
<?php
session_start();
echo "var h_contract=\"$_SESSION[H_contract]\";\n";
?>
document.addEventListener("DOMContentLoaded", function (e) {
  var eve = new CustomEvent('taler-execute-payment', {detail: {H_contract: h_contract}});
  document.dispatchEvent(eve);
});
document.addEventListener("taler-payment-result", function (e) {
  console.log("finished payment");
});
    </script>
</head>

<body>
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="black" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="black">S</text>
      </svg>
    </div>
    <h1>Toy Store - Taler Demo</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
      <h1>Executing Payment ...</h1>
      <div class="loader">Loading...</div>
</body>
</html>
