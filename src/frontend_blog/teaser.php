<html>
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

/**
 * This file should:
 *
 * 1. fetch the article teaser and attach a 'read more' link to it
 *
 */
$article = (isset($_GET['article']) ? $_GET['article'] : "No article");
$content = file_get_contents("articles/$article.html");
$doc = new DOMDocument();
$doc->loadHTML($content);
$teaser = $doc->getElementById("teaser");
?>
<head>
  <title>
    <?php echo $article ?>
  </title>
</head>
<body onload="signal_taler_wallet_onload();">
<?php if ($article == "No article")
        echo "Please select some article";
      else {
        session_start();
	$_SESSION['article'] = $article;
	echo $teaser->nodeValue;
      }
?>
	<br><a href="cc_payment.html" id="read-more">read more</a>

</body>

</html>
