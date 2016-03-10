<!DOCTYPE html>
<!-- 
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
-->
<?php
require_once "../../copylib/config.php";
?>
<html lang="en">
<head>
  <title>Taler Donation Demo</title>
  <link rel="stylesheet" type="text/css" href="web-common/style.css">
  <script src="web-common/taler-presence.js" type="text/javascript"></script>
<script type="text/javascript">
<?php
echo "\tvar shop_currency = '$MERCHANT_CURRENCY';\n";
?>

  function addOption(value, label) {
    var s = document.getElementById("taler-donation");
    var e = document.createElement("option");
    e.textContent = label ? label : ("".concat(value, " ", shop_currency));
    e.value = value;
    s.appendChild(e);
  }

  function init() {
    var e = document.getElementById("currency-input");
    e.value = shop_currency;
    addOption("0.1");
    addOption("1.0");
    addOption("6.0", "".concat(5, " ", shop_currency));
    addOption("10");
  }

  document.addEventListener("DOMContentLoaded", init);

</script>
</head>

<body>
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">S</text>
      </svg>
    </div>
    <!--#include virtual="web-common/dropdown-navbar.html" -->
    <h1 class="nav">Toy &quot;Store&quot; - Taler Demo</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
      <h1>Welcome to the Taler Donation "Shop" Demo</h1>

      <p>This "toy" website provides you with the ability to
        experience using the
        <a href="https://www.taler.net/">GNU Taler</a>
        payment system without using
        valuable currency. Instead, for the demonstrator we
        will be using a "toy" currency, KUDOS. However, please remember that
        Taler is designed to work with ordinary currencies, such
        as Dollars or Euros, not just toy currencies.
        <br>
        This page, <tt>shop.demo.taler.net</tt> models the behavior of a
        typical Web shop supporting Taler.  The other pages of the demo,
        <tt>exchange.demo.taler.net</tt> and
        <tt>bank.demo.taler.net</tt>, correspond to a Taler exchange
        and bank with tight Taler integration respectively.  You
        may also enjoy visiting the <tt>blog.demo.taler.net</tt>.
      </p>
    </article>

    <div class="taler-installed-hide">
      <h2>Installing the Taler wallet</h2>
      First, you need to install the Taler wallet browser extension.
      Install the wallet
      <span id="install-done" style="visibility: hidden">(done)</span>
      <ul>
        <li>from the app store for <a href="https://chrome.google.com/webstore/detail/gnu-taler-wallet/millncjiddlpgdmkklmhfadpacifaonc">Google
            Chrome and Chromium</a>
        </li>
        <li>via inline install:
          <button onclick="installWallet()" id="install-button">
            Add to Chrome
          </button>
        </li>
      </ul>
      Wallets for other browsers will be provided in the near future.
    </div>

    <div class="taler-installed-show">
      <p>Please choose a project and the amount of KUDOS you
        wish to donate:</p>

      <form name="tform" action="checkout.php" method="POST">
        <div class="participation" id="fake-shop">
          <br>
          <input type="radio" name="donation_receiver" value="Taler" checked="true">GNU Taler</input>
          <br>
          <input type="radio" name="donation_receiver" value="Tor">Tor</input>
          <br>
          <input type="radio" name="donation_receiver" value="GNUnet">GNUnet</input>
          <br>
          <select id="taler-donation" name="donation_amount">
            <!-- options will be added dynamically -->
          </select>
          <input id="currency-input" type="hidden" name="donation_currency"/>
          <input type="submit" name="keyName" value="Donate!"/>
          <br>
          <br>
        </div>
      </form>
      <p>
        (*) To make it a bit more fun, the 5 KUDOS option
        is deliberately implemented with a fault: the merchant will try to
        make you donate 6 KUDOS instead of the 5 KUDOS you got to see.  But do
        not worry, you will be given the opportunity to review the
        final offer from the merchant in a window secured
        by the Taler extension.  That way, you can spot the
        error before committing to an incorrect contract.
      </p>
    </div>
  </section>
</body>
</html>
