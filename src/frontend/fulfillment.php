<!DOCTYPE html>
<html lang="en">
<head>
  <title>Taler's "Demo" Shop</title>
  <link rel="stylesheet" type="text/css" href="style.css">
</head>
<body>

  <header>
    <div id="logo">
      <svg height="100" width="100">
        <circle cx="50" cy="50" r="40" stroke="darkcyan" stroke-width="6" fill="white" />
        <text x="19" y="82" font-family="Verdana" font-size="90" fill="darkcyan">S</text>
      </svg>
    </div>

    <h1>Toy Store - Payment succeeded</h1>
  </header>

  <aside class="sidebar" id="left">
  </aside>

  <section id="main">
    <article>
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

function generate_msg ($link){
  $msg = "<p>Thanks for donating to " . $_SESSION['receiver'] . ".</p>";
  if (false != $link)
    $msg .= "<p>Check our latest <a href=\"" . $link . "\">news!</a></p>";
  return $msg;
}

session_start();

if (!isset ($_SESSION['payment_ok']))
{
  echo "<p>Please come here after a successful payment!</p>";
}
else
{
  $news = false;
  switch ($_SESSION['receiver'])
  {
    case "Taler":
      $news = "https://taler.net/news";
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
    </article>
  </section>
</body>
</html>
