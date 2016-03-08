<?php
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

function get(&$var, $default=null) {
  return isset($var) ? $var : $default;
}

function &pull(&$arr, $idx, $default) {
  if (!isset($arr[$idx])) {
    $arr[$idx] = $default;
  }
  return $arr[$idx];
}

function message_from_missing_param($missing, $link, $link_name="home page"){
  return "<p>Bad request, no $missing given. Return to <a href=\"$link\">$link_name</a></p>";
}

function article_state_to_str($article_state){
  if(null == $article_state || !isset($article_state))
    return "undefined state";
  $str = "Is payed? ";
  $str .= $article_state['ispayed'] ? "true," : "false,";
  if(!isset($article_state['hc']))
    $str .= " no hashcode for this article";
  else $str .= " " . $article_state['hc'];
  return $str;
}

function log_string($str){
  file_put_contents("/tmp/blog.dbg", $str . "\n", FILE_APPEND);
}

function get_full_uri(){

  return $_SERVER['REQUEST_SCHEME'] . '://'
	 . $_SERVER['HTTP_HOST']
         . $_SERVER['REQUEST_URI'];
}

function url_join($base, $path, $strip=false) {
  $flags = $strip ? (http\Url::STRIP_PATH|http\URL::STRIP_QUERY) : 0;
  return (new http\URL($base, null, $flags))
    ->mod(array ("path" => $path), http\Url::JOIN_PATH|http\URL::SANITIZE_PATH)
    ->toString();
}

// Get a url with a path relative to the
// current script's path.
function url_rel($path, $strip=false) {
  return url_join(
    $_SERVER['REQUEST_SCHEME'] . '://' . $_SERVER['HTTP_HOST'].$_SERVER['REQUEST_URI'],
    $path,
    $strip);
}

function template($file, $array) {
  if (file_exists($file)) {
    $output = file_get_contents($file);
    foreach ($array as $key => $val) {
      $replace = '{'.$key.'}';
      $output = str_replace($replace, $val, $output);
    }
    return $output;
  }
}

function str_to_dom($str){
  $doc = new DOMDocument();
  $doc->loadHTML($str);
  return $doc;

}
?>
