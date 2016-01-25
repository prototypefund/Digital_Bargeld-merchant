<?php

function get(&$var, $default=null) {
  return isset($var) ? $var : $default;
}

function &pull(&$arr, $idx, $default) {
  if (!isset($arr[$idx]) {
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
?>
