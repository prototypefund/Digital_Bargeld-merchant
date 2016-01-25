<?php

function get(&$var, $default=null) {
  return isset($var) ? $var : $default;
}

function url_join($base, $path) {
  $url = (new http\URL($base))
    ->mod(array ("path" => $path), http\Url::JOIN_PATH|http\URL::SANITIZE_PATH);
  return $url->toString();
}

// Get a url with a path relative to the
// current script's path.
function url_rel($path) {
  return url_join(
    $_SERVER['REQUEST_SCHEME'] . '://' . $_SERVER['HTTP_HOST'].$_SERVER['REQUEST_URI'],
    $path);
}
?>
