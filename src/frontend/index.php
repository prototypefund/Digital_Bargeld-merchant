<!DOCTYPE html>
<?php
require_once "config.php";
?>
<html lang="en">
<head>
  <title>Toy &quot;Store&quot; - Taler Demo</title>
  <link rel="stylesheet" type="text/css" href="style.css">
  <script src="taler-presence.js" type="text/javascript"></script>
<script type="text/javascript">
<?php
echo "\tvar shop_currency = '$SHOP_CURRENCY';\n";
?>

  function addOption(value, label) {
    var s = document.getElementById("taler-donation");
    let e = document.createElement("option");
    e.textContent = label ? label : ("".concat(value, " ", shop_currency));
    e.value = value;
    s.appendChild(e);
  }

  function init() {
    let e = document.getElementById("currency-input"); 
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

    <h1>Toy &quot;Store&quot; - Taler Demo</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
      <h1>Welcome to the Taler Demo Shop</h1>

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
        <tt>mint.demo.taler.net</tt> and
        <tt>bank.demo.taler.net</tt>, correspond to a Taler mint
        and bank with tight Taler integration respectively.
      </p>
    </article>

    <section>

      <article>
        <h2>Step 1: Installing the Taler wallet</h2>
        <p class="taler-installed-hide">
          First, you need to <a href="http://demo.taler.net/">install</a>
          the Taler wallet browser extension.
        </p>
        <p class="taler-installed-show">
          Congratulations, you have installed the Taler wallet correctly.
          You can now proceed with the next steps.
        </p>
      </article>

      <article class="taler-installed-show">
        <h2>Step 2: Withdraw coins <sup>(occasionally)</sup></h2>

        <p>The next step is to withdraw coins, after all you cannot
          pay with an empty wallet.  To be allowed to withdraw
          coins from a mint, you first need to transfer currency to the mint
          using the normal banking system, for example by using a
          wire transfer.  If the bank offers a tight integration with Taler, it
          may also support this directly over the home banking online interface.
          <br>
          For the demonstration, we have created a "bank" that
          allows you to "wire" funds (in KUDOS) to the mint simply by
          filling in the desired amount into a form.  Naturally, when
          using a real bank with real money, you would have to authenticate
          and authorize the transfer.
          <br>
          Note that you would not do this step for each purchase or each shop.
          Payment with Taler is like paying
          with cash: you withdraw currency at the bank (or an ATM) and then
          pay at many merchants without having to authenticate each time.
          <br>
          So, unless you have already done so, please go ahead and withdraw
          KUDOS at the
          <a href="http://bank.demo.taler.net/" target="_blank">Demo bank</a>
	  (opens in a new tab).</p>
      </article>

      <article class="taler-installed-show">
        <h2>Step 3: Shop! <sup>(as long as you have KUDOS left)</sup></h2>

        <p>Now it is time to spend your hard earned KUDOS.
          Note that we cannot really tell if you got any yet,
          as your Taler wallet balance is visible to you, but
          of course is hidden entirely from the shop.</p>
        <p>The form that follows corresponds to the shopping
          cart of a real Web shop; however, we kept it
          very simple for the demonstration.</p>
        <p>So, please choose a project and the amount of KUDOS you
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
        <p>(*) To make it a bit more fun, the 5 KUDOS option
          is deliberately implemented with a fault: the merchant will try to
          make you donate 6 KUDOS instead of the 5 KUDOS you got to see.  But do
          not worry, you will be given the opportunity to review the
          final offer from the merchant in a window secured
          by the Taler extension.  That way, you can spot the
          error before committing to an incorrect contract.</p>
      </article>
    </section>
  </section>
</body>
</html>
