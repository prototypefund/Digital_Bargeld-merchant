<?php

session_start();

if (!isset($_GET['article'])){
  http_response_code(400);
  echo "No article specified";
  die();
}
$article = $_GET['article']; 
/* check if the client is allowed to get the wanted article */
if(!isset($_SESSION['allowed_articles'][$article])){
  http_response_code(401); // unauthorized
  echo "Not allowed to read this article";
  die();
}
// get the article
$article_doc = get_article($article);
echo $article_doc->saveHTML();
?>
