<?php
  /**
   * Parse $html_filename and add an entry of the type
   * "$html_filename" => ("img1.png", "img2.png") for each
   * encountered 'img' tag having the 'src' attribute formatted
   * as "/essay/<article_slug>/data/img1.png", to the JSON which
   * associates any article with its images
   */
  function add_article($html_filename){
    $doc = new DOMDocument(); 
    $doc->loadHTMLFile($html_filename);
    $xpath = new DOMXPath($doc);
    $xpath->registerNamespace('php', 'http://php.net/xpath');
    $xpath->registerPhpFunctions('preg_match');
    $elements = $xpath->query('//img[php:functionString("preg_match", "@^/essay/[^/]+/data/[^/]+@", @src) > 0]');
    /*
    1 open final JSON in "append mode"
    2 append current file's entries
    */
    $db_filename = "articles_images.json";
    $json_str;
    if (file_exists($db_filename))
      $json_str = file_get_contents($db_filename);
    else
      $json_str = "";
    $json_db = json_decode($json_str);
    $json_db->$html_filename = array();
    foreach($elements as $img){
      $value = $img->getAttributeNode("src")->value;
      array_push($json_db->$html_filename, basename($value));
    }
    file_put_contents($db_filename, json_encode($json_db));
  }

  /* suppress warnings due to parsing HTML5 */
  libxml_use_internal_errors(true);

  /* main: manually call add_article() for each article */
  add_article("scrap1_10.html");
  add_article("scrap1_11.html");
?>
