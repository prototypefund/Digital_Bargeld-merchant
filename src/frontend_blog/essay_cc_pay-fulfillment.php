<?php

include '../frontend_lib/util.php';
include './blog_lib.php';

session_start();
if (!$_SESSION['cc_payment'])
{
  echo "No session active";
  die();
}
$article = get($_GET['article']);
if (null == $article)
{
  http_response_code(400);
  echo "Bad request (no article specified)";
  return;
}
$article_doc = get_article($article);
echo $article_doc->saveHTML();

?>
