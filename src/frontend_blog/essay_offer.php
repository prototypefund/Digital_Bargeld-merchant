<!DOCTYPE html>
<html>
<body onload="signal_taler_wallet_onload()">
<?php
  
  include("../frontend_lib/merchants.php");
  include("../frontend_lib/util.php");
  include("./blog_lib.php");
  $article = get($_GET['article']);
  if (null == $article){
    echo "Please land here just to buy articles";
    die();
    }
  $cc_page = template("./essay_cc-payment.html", array('article' => $article));
  echo $cc_page;
?>
</body>
<script type="text/javascript">

  function handle_contract(json_contract) {
    var cEvent = new CustomEvent('taler-contract',
                                 {detail: json_contract});
    document.dispatchEvent(cEvent);
  };

  function get_contract(article) {
  var contract_request = new XMLHttpRequest();

  contract_request.open("GET",
                        "essay_contract.php?article=" + article,
			true);
  contract_request.onload = function (e) {
    if (contract_request.readyState == 4) {
      if (contract_request.status == 200) {
        console.log("response text:",
	            contract_request.responseText);
        handle_contract(contract_request.responseText);
      } else {
        alert("Failure to download contract from merchant " +
              "(" + contract_request.status + "):\n" +
              contract_request.responseText);
      }
    }
  };
  contract_request.onerror = function (e) {
    alert("Failure requesting the contract:\n"
          + contract_request.statusText);
  };
  contract_request.send();
  }

  function has_taler_wallet_cb(aEvent)
  {
    var article = document.getElementById('article-name');
    get_contract(article.value); 
  };
  
  function signal_taler_wallet_onload()
  {
    var eve = new Event('taler-probe');
    document.dispatchEvent(eve);
  };
  
  document.addEventListener("taler-wallet-present",
                            has_taler_wallet_cb,
  			  false);
  
  // Register event to be triggered by the wallet when it gets enabled while
  // the user is on the payment page
  document.addEventListener("taler-load",
                            signal_taler_wallet_onload,
                            false);

</script>
</html>
