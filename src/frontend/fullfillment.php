<!DOCTYPE html>
<html lang="en">
<head>
  <title>Taler's "Demo" Shop</title>
  <link rel="stylesheet" type="text/css" href="style.css">
</head>
<body>

<?php 
/*
  This file is part of GNU TALER.
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>

*/

$cli_debug = false;
$backend_test = true;

function generate_msg ($link){
  $msg = "Thanks for donating to " . $_SESSION['receiver'] . ".";
  if (false != $link)
    $msg .= " Check our latest <a href=\"" . $link . "\">news!</a>";
  return $msg;
}

if ($_GET['cli_debug'] == 'yes')
  $cli_debug = true;

if ($_GET['backend_test'] == 'no')
{
  $cli_debug = true;
  $backend_test = false;
}

session_start();

if (! isset ($_SESSION['payment_ok']))
  echo "Please land here after a successful payment!";
else{
  $news = false;
  switch ($_SESSION['receiver']){
    case "Taler":
      $news = "https://taler.net/about";
      break;
    case "GNUnet":
      $news = "https://gnunet.org/";
      break;
    case "Tor":
      $news = "https://www.torproject.org/press/press.html.en";
      break;
 }
  echo generate_msg ($news); 
}

?>

</body>
</html>
