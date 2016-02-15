<!DOCTYPE html>
<html lang="en">
<head>
  <title>Enter your details</title>
  <link rel="stylesheet" type="text/css" href="style.css">
</head>
<?php
  if (!isset($_GET['article'])){
    echo "Please select an article to buy";
    die();
  }
  else {
    session_start();
    $_SESSION['cc_payment'] = true;
    $article = $_GET['article'];
    }
?>

<body>
  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">B</text>
      </svg>
    </div>

    <h1>Blog site demonstration</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
      <h1>Enter your details</h1>
      <p>We need a few details before proceeding with credit card payment</p>
    <form>
      First name<br> <input type="text"></input><br>
      Family name<br> <input type="text"></input><br>
      Age<br> <input type="text"></input><br>
      Nationality<br> <input type="text"></input><br>
      Gender<br> <input type="radio" name"gender">Male</input>
             <input type="radio" name="gender">Female</input><br>
    </form>
    <?php
      echo "<form method=\"post\" action=\"essay_cc_pay-fulfillment.php?article=$article\"><input type=\"submit\"></input></form>";
    ?>
    </article>
  </section>
</body>
</html>
