<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

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
