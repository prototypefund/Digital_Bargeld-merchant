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

  add_article("scrap1_10.html");
  add_article("scrap1_11.html");
  add_article("scrap1_12.html");
  add_article("scrap1_13.html");
  add_article("scrap1_14.html");
  add_article("scrap1_15.html");
  add_article("scrap1_16.html");
  add_article("scrap1_17.html");
  add_article("scrap1_18.html");
  add_article("scrap1_19.html");
  add_article("scrap1_1.html");
  add_article("scrap1_20.html");
  add_article("scrap1_21.html");
  add_article("scrap1_22.html");
  add_article("scrap1_23.html");
  add_article("scrap1_24.html");
  add_article("scrap1_25.html");
  add_article("scrap1_26.html");
  add_article("scrap1_27.html");
  add_article("scrap1_28.html");
  add_article("scrap1_29.html");
  add_article("scrap1_2.html");
  add_article("scrap1_30.html");
  add_article("scrap1_31.html");
  add_article("scrap1_32.html");
  add_article("scrap1_33.html");
  add_article("scrap1_34.html");
  add_article("scrap1_35.html");
  add_article("scrap1_36.html");
  add_article("scrap1_37.html");
  add_article("scrap1_38.html");
  add_article("scrap1_39.html");
  add_article("scrap1_3.html");
  add_article("scrap1_40.html");
  add_article("scrap1_41.html");
  add_article("scrap1_42.html");
  add_article("scrap1_43.html");
  add_article("scrap1_46.html");
  add_article("scrap1_47.html");
  add_article("scrap1_4.html");
  add_article("scrap1_5.html");
  add_article("scrap1_6.html");
  add_article("scrap1_7.html");
  add_article("scrap1_8.html");
  add_article("scrap1_9.html");
  add_article("scrap1_U.0.html");
  add_article("scrap1_U.1.html");
?>
