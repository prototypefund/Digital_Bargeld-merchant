<!DOCTYPE html>
<html>
<head>
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
</script>
</head>
<body>
<?php
  
  include("../frontend_lib/merchants.php");
  include("../frontend_lib/util.php");
  include("./blog_lib.php");
  session_start();
  $article = get($_GET['article']);
  if (null == $article){
    echo "Please land here just to buy articles";
    die();
    }
  echo "<script>get_contract('$article');</script>"
?>
</body>
</html>
