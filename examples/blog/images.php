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

  include '../../copylib/util.php';

  $article = get($_GET['article']);
  $image = get($_GET['image']);
  session_start();
  $payments = &pull($_SESSION, 'payments', false);
  if (!$payments) {
    echo "No session active"; 
    return 400;
  }
  if (null == get($payments[$article]['ispayed'])) {
    echo "Article not payed";
    return 400;
  }

  $db_filename = "articles/articles_images.json";
  $json_str;
  if (file_exists($db_filename))
    $json_str = file_get_contents($db_filename);
  else {
    echo "Internal server error: data registry not found"; 
    return 500;  
  }
  $db = json_decode($json_str, true);
  $article_images = get($db[$article . ".html"]);
  if (null == $article_images) {
    echo "This article has no images to sell"; 
    return 400;
  }
  if (false === array_search($image, $article_images)) {
    echo "Requested image does not belong to article '$article'";
    return 400;
  }

  $image_path = "data/" . $image;
  $fp = fopen($image_path, 'rb');
  header("Content-Type: image/png"); // fix image extension
  header("Content-Length: " . filesize($image_path));
  fpassthru($fp);
  exit;
?>
