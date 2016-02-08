<?php
session_start();
if (!$_SESSION['cc_payment'])
{
  echo "No session active";
  die();
}
$article = $_GET['article'];
$_SESSION['payment_ok'] = true;
if (!isset($_SESSION['allowed_articles']))
  $_SESSION['allowed_articles'] = array ($article => true);
else $_SESSION['allowed_articles'] = array_merge($_SESSION['allowed_articles'], array ($article => true));
http_response_code (301);
$url =  'http://' . $_SERVER['HTTP_HOST'] . "/essay_cc_fulfillment.php?article=$article";
header("Location: " . $url);
echo $url;
die();
?>
