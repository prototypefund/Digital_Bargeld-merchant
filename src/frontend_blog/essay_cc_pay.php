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
// WARNING: the following construct yields non-expected forms when using URL
// parameter in it
//$url = (new http\URL($_SERVER['REQUEST_SCHEME'] . '://' . $_SERVER['HTTP_HOST']))
//  ->mod(array ("path" => "essay_fulfillment.php?article=$article"), http\Url::JOIN_PATH);
$url =  'http://' . $_SERVER['HTTP_HOST'] . "/essay_fulfillment.php?article=$article";
header("Location: " . $url);
echo $url;
die();
?>
