<?php
  /**
   * Take a (article's) filename and return its
   * teaser. It has the articles folder hardcoded
   */
  function get_title($name){
    $content = file_get_contents("articles/$name.html");
    $doc = new DOMDocument();
    $doc->loadHTML($content);
    $finder = new DOMXPath($doc);
    $query_set = $finder->query("//h1[@class='chapter' or @class='unnumbered']");
    if (1 != $query_set->length)
      return "No title for this item";
    // assuming all the articles are well-formed..
    return $query_set->item(0)->nodeValue;
  }
  
  /**
   * Take a (article's) filename and return its
   * DOM. It has the articles folder hardcoded
   */
  function get_article($name){
    $raw_content = file_get_contents("articles/$name.html");
    return $raw_content;
  }
  
  /**
   * Fetch the page $page and return its
   * DOM.
   */
  function get_page($page){
    $content = file_get_contents($page);
    $doc = new DOMDocument();
    $doc->loadHTML($content);
    return $doc;
  }
?>
