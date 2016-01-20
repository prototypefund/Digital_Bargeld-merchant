<?php

/**
 * Take a (article's) filename and return its
 * teaser. It has the articles folder hardcoded
 */
function get_teaser($name){
  $content = file_get_contents("articles/$name.html");
  $doc = new DOMDocument();
  $doc->loadHTML($content);
  $teaser = $doc->getElementById("teaser");
  return $teaser;
}

/**
 * Take a (article's) filename and return its
 * DOM. It has the articles folder hardcoded
 */
function get_article($name){
  $content = file_get_contents("articles/$name.html");
  $doc = new DOMDocument();
  $doc->loadHTML($content);
  return $doc;
}

?>
