<?php
  include '../../copylib/util.php';
  include './blog_lib.php';

  $article = get($_GET['article']);
  if (null == $article){
    http_response_code(400);
    echo "Bad request (no article specified)";
    return;
  }

  session_start();
  $payments = &pull($_SESSION, "payments", array());
  $payments[$article] = array("ispayed" => true);
  $fulfillment_url = url_rel("essay_fulfillment.php");
  header("Location: $fulfillment_url");
  die();
?>
