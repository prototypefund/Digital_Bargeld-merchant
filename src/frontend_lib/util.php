<?php

function get(&$var, $default=null) {
  return isset($var) ? $var : $default;
}

function &pull(&$arr, $idx, $default) {
  if (!isset($arr[$idx])) {
    $arr[idx] = $default;
  }
  return $arr[$idx];
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
